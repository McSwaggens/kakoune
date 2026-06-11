#include "lsp_client.hh"

#include "buffer_utils.hh"
#include "file.hh"
#include "format.hh"
#include "option_manager.hh"
#include "scope.hh"
#include "string_utils.hh"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace Kakoune
{

namespace
{

Optional<ByteCount> find_substr(StringView haystack, StringView needle)
{
    auto found = std::search(haystack.begin(), haystack.end(),
                             needle.begin(), needle.end());
    if (found == haystack.end())
        return {};
    return ByteCount{(int)(found - haystack.begin())};
}

String frame(StringView body)
{
    return "Content-Length: " + to_string(body.length()) + "\r\n\r\n" + body;
}

}

bool lsp_debug_enabled()
{
    try { return GlobalScope::instance().options()["lsp_debug"].get<bool>(); }
    catch (runtime_error&) { return false; }
}

LSPClient::LSPClient(StringView cmdline, const Context& spawn_ctx,
                     NotificationHandler on_notification, RequestHandler on_request)
    : m_shell{ShellManager::instance().spawn(cmdline, spawn_ctx, true)},
      m_on_notification{std::move(on_notification)},
      m_on_request{std::move(on_request)}
{
    // stdin is written non-blocking and drained by the event loop.
    int in_fd = (int)m_shell.in;
    fcntl(in_fd, F_SETFL, fcntl(in_fd, F_GETFL, 0) | O_NONBLOCK);

    m_out_watcher.emplace((int)m_shell.out, FdEvents::Read, EventMode::Urgent,
        [this](FDWatcher&, FdEvents, EventMode) { on_readable(); });

    m_err_watcher.emplace((int)m_shell.err, FdEvents::Read, EventMode::Urgent,
        [this](FDWatcher& watcher, FdEvents, EventMode) {
            const int fd = watcher.fd();
            // Server stderr (e.g. clangd's verbose logging) is only surfaced
            // when lsp_debug is set; otherwise it is read and discarded.
            const bool keep = lsp_debug_enabled();
            char buffer[1024];
            while (fd_readable(fd))
            {
                ssize_t size = ::read(fd, buffer, sizeof(buffer));
                if (size <= 0)
                {
                    if (size < 0 and errno == EAGAIN)
                        continue;
                    watcher.disable();
                    return;
                }
                if (keep)
                    m_stderr_buf += StringView{buffer, buffer + size};
            }
            // Emit complete lines to the debug buffer.
            while (keep)
            {
                auto nl = find_substr(m_stderr_buf, "\n");
                if (not nl)
                    break;
                log(StringView{m_stderr_buf}.substr(0_byte, *nl));
                m_stderr_buf = String{StringView{m_stderr_buf}.substr(*nl + 1)};
            }
        });

    m_in_watcher.emplace(in_fd, FdEvents::None, EventMode::Urgent,
        [this](FDWatcher&, FdEvents, EventMode) { on_writable(); });
}

LSPClient::~LSPClient()
{
    // FDWatchers unregister here; m_shell closes the pipes and SIGTERMs the
    // process via its UniquePid. Graceful shutdown/exit is the manager's job.
}

void LSPClient::log(StringView what)
{
    write_to_debug_buffer(format("lsp: {}", what));
}

void LSPClient::mark_dead()
{
    if (m_dead)
        return;
    m_dead = true;
    if (m_out_watcher) m_out_watcher->disable();
    if (m_in_watcher) m_in_watcher->disable();
    if (m_err_watcher) m_err_watcher->disable();

    // Fail every in-flight request so no caller waits forever.
    auto pending = std::move(m_pending);
    m_pending = {};
    for (auto&& entry : pending)
        entry.value(Value{}, Value{String{"language server terminated"}});
}

void LSPClient::on_readable()
{
    const int fd = (int)m_shell.out;
    char buffer[4096];
    while (fd_readable(fd))
    {
        ssize_t size = ::read(fd, buffer, sizeof(buffer));
        if (size <= 0)
        {
            if (size < 0 and errno == EAGAIN)
                continue;
            mark_dead();
            return;
        }
        m_read_buf += StringView{buffer, buffer + size};
    }
    drain_messages();
}

void LSPClient::drain_messages()
{
    while (true)
    {
        if (not m_expected_len)
        {
            auto sep = find_substr(m_read_buf, "\r\n\r\n");
            if (not sep)
                return; // header block not complete yet

            StringView header = StringView{m_read_buf}.substr(0_byte, *sep);
            auto cl = find_substr(header, "Content-Length:");
            if (cl)
            {
                auto p = header.begin() + (int)(*cl) + (int)StringView{"Content-Length:"}.length();
                while (p != header.end() and (*p == ' ' or *p == '\t'))
                    ++p;
                auto digits = p;
                while (p != header.end() and *p >= '0' and *p <= '9')
                    ++p;
                m_expected_len = (size_t)str_to_int(StringView{digits, p});
            }
            else
                log("message without Content-Length header, skipping");

            m_read_buf = String{StringView{m_read_buf}.substr(*sep + 4)};
            if (not m_expected_len)
                continue;
        }

        if ((size_t)(int)m_read_buf.length() < *m_expected_len)
            return; // body not fully received yet

        StringView body = StringView{m_read_buf}.substr(0_byte, ByteCount{(int)*m_expected_len});
        Value message;
        try
        {
            message = parse_json(body).value;
        }
        catch (runtime_error& err)
        {
            log(format("failed to parse message: {}", err.what()));
        }
        m_read_buf = String{StringView{m_read_buf}.substr(ByteCount{(int)*m_expected_len})};
        m_expected_len = {};

        if (message)
            dispatch(std::move(message));
    }
}

void LSPClient::dispatch(Value message)
{
    if (not message.is_a<JsonObject>())
        return;
    auto& obj = message.as<JsonObject>();

    auto id_it = obj.find("id"_sv);
    auto method_it = obj.find("method"_sv);
    const bool has_id = id_it != obj.end() and (bool)id_it->value;
    const bool has_method = method_it != obj.end();

    if (has_id and not has_method)
    {
        // Response to one of our requests.
        if (not id_it->value.is_a<int>())
            return; // we only ever send integer ids
        int id = id_it->value.as<int>();
        auto pending = m_pending.find(id);
        if (pending == m_pending.end())
            return;
        ResponseCallback cb = std::move(pending->value);
        m_pending.remove(id);

        Value result, error;
        if (auto it = obj.find("result"_sv); it != obj.end())
            result = std::move(it->value);
        if (auto it = obj.find("error"_sv); it != obj.end())
            error = std::move(it->value);
        cb(std::move(result), std::move(error));
    }
    else if (has_method)
    {
        StringView method = method_it->value.as<String>();
        Value params;
        if (auto it = obj.find("params"_sv); it != obj.end())
            params = std::move(it->value);

        if (has_id)
        {
            // server->client request: let the handler produce a result (e.g.
            // workspace/applyEdit), otherwise ack with null.
            Value result = m_on_request ? m_on_request(method, params) : Value{};
            queue_write(frame("{\"jsonrpc\":\"2.0\",\"id\":" + to_json(id_it->value) +
                              ",\"result\":" + to_json(result) + "}"));
        }
        else
            m_on_notification(method, std::move(params));
    }
}

int LSPClient::send_request(StringView method, String params_json, ResponseCallback cb)
{
    int id = m_next_id++;
    if (m_dead)
    {
        cb(Value{}, Value{String{"language server terminated"}});
        return id;
    }
    m_pending.insert({id, std::move(cb)});
    queue_write(frame("{\"jsonrpc\":\"2.0\",\"id\":" + to_string(id) +
                      ",\"method\":" + to_json(method) +
                      ",\"params\":" + params_json + "}"));
    return id;
}

void LSPClient::send_notification(StringView method, String params_json)
{
    if (m_dead)
        return;
    queue_write(frame("{\"jsonrpc\":\"2.0\",\"method\":" + to_json(method) +
                      ",\"params\":" + params_json + "}"));
}

void LSPClient::queue_write(String framed)
{
    if (m_dead)
        return;
    m_write_buf += framed;
    if (m_in_watcher)
        m_in_watcher->events() = FdEvents::Write;
    on_writable(); // try to flush immediately
}

void LSPClient::on_writable()
{
    const int fd = (int)m_shell.in;
    while (not m_write_buf.empty() and fd_writable(fd))
    {
        ssize_t size = ::write(fd, m_write_buf.begin(), (size_t)(int)m_write_buf.length());
        if (size > 0)
            m_write_buf = String{StringView{m_write_buf}.substr(ByteCount{(int)size})};
        else if (size < 0 and (errno == EAGAIN or errno == EWOULDBLOCK))
            break;
        else
        {
            mark_dead();
            return;
        }
    }
    if (m_in_watcher)
        m_in_watcher->events() = m_write_buf.empty() ? FdEvents::None : FdEvents::Write;
}

}
