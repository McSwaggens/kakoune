#include "fim.hh"

#include "buffer.hh"
#include "buffer_manager.hh"
#include "buffer_utils.hh"
#include "command_manager.hh"
#include "context.hh"
#include "file.hh"
#include "highlighters.hh" // RangeAndStringList, InclusiveBufferRange
#include "hook_manager.hh"
#include "json.hh"
#include "keymap_manager.hh"
#include "option_manager.hh"
#include "ranges.hh"
#include "regex.hh"
#include "scope.hh"
#include "selection.hh"
#include "shell_manager.hh"
#include "string_utils.hh"
#include "unit_tests.hh"
#include "utf8.hh"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Kakoune
{

namespace
{

bool fim_debug_enabled()
{
    try { return GlobalScope::instance().options()["fim_debug"].get<bool>(); }
    catch (runtime_error&) { return false; }
}

// Always written (errors and other things the user should see).
void debug(StringView msg) { write_to_debug_buffer(format("fim: {}", msg)); }
// Verbose traffic/info; only written when the fim_debug option is set.
void trace(StringView msg) { if (fim_debug_enabled()) write_to_debug_buffer(format("fim: {}", msg)); }

Optional<ByteCount> find_substr(StringView haystack, StringView needle)
{
    auto found = std::search(haystack.begin(), haystack.end(),
                             needle.begin(), needle.end());
    if (found == haystack.end())
        return {};
    return ByteCount{(int)(found - haystack.begin())};
}

String trim_prefix_to_bytes(String text, int max_bytes)
{
    if (max_bytes <= 0 or text.length() <= ByteCount{max_bytes})
        return text;

    const char* end = text.begin() + ByteCount{max_bytes};
    while (end != text.begin() and not utf8::is_character_start(*end))
        --end;
    return String{StringView{text.begin(), end}};
}

String trim_suffix_to_bytes(String text, int max_bytes)
{
    if (max_bytes <= 0 or text.length() <= ByteCount{max_bytes})
        return text;

    const char* begin = text.end() - max_bytes;
    begin = utf8::finish(begin, text.end());
    return String{StringView{begin, text.end()}};
}

const Value* find_member(const Value& v, StringView key)
{
    if (not v.is_a<JsonObject>())
        return nullptr;
    auto& o = v.as<JsonObject>();
    auto it = o.find(key);
    return it != o.end() ? &it->value : nullptr;
}

template<typename T>
void set_buffer_option(Buffer& buffer, StringView name, const T& value)
{
    buffer.options().get_local_option(name).set<T>(value);
}

// ── HTTP, the minimal subset llama-server needs ─────────────────────────────

struct HttpUrl
{
    String host; // numeric IPv4
    int port;
};

// http://<ipv4|localhost>[:port][/]; name resolution would block the event
// loop, so only numeric hosts (and the literal "localhost") are accepted.
Optional<HttpUrl> parse_http_url(StringView url)
{
    constexpr StringView scheme = "http://";
    if (not url.starts_with(scheme))
        return {};
    StringView rest = url.substr(scheme.length());
    rest = StringView{rest.begin(), find(rest, '/')};
    auto colon = find(rest, ':');
    StringView host{rest.begin(), colon};
    if (host.empty())
        return {};
    int port = 80;
    if (colon != rest.end())
    {
        StringView port_str{colon + 1, rest.end()};
        if (port_str.empty())
            return {};
        for (char c : port_str)
            if (c < '0' or c > '9')
                return {};
        port = str_to_int(port_str);
        if (port <= 0 or port > 65535)
            return {};
    }
    return HttpUrl{host == "localhost" ? String{"127.0.0.1"} : String{host}, port};
}

// Connection: close keeps the response framing trivial: when the server sends
// no Content-Length, the body simply ends at EOF.
String build_http_post(StringView host, int port, StringView path, StringView body)
{
    return format("POST {} HTTP/1.1\r\n"
                  "Host: {}:{}\r\n"
                  "Content-Type: application/json\r\n"
                  "Accept: application/json\r\n"
                  "Content-Length: {}\r\n"
                  "Connection: close\r\n"
                  "\r\n{}",
                  path, host, port, (int)body.length(), body);
}

struct HttpResponse
{
    int status;
    String body;
};

bool iequal(StringView a, StringView b)
{
    auto lower = [](char c) { return c >= 'A' and c <= 'Z' ? (char)(c - 'A' + 'a') : c; };
    if (a.length() != b.length())
        return false;
    for (auto ai = a.begin(), bi = b.begin(); ai != a.end(); ++ai, ++bi)
        if (lower(*ai) != lower(*bi))
            return false;
    return true;
}

// Returns disengaged while more data is needed; throws on malformed input
// (including input truncated at eof).
Optional<HttpResponse> parse_http_response(StringView data, bool eof)
{
    auto header_end = find_substr(data, "\r\n\r\n");
    if (not header_end)
    {
        if (eof)
            throw runtime_error("truncated http response");
        return {};
    }
    StringView headers = data.substr(0_byte, *header_end);
    StringView body = data.substr(*header_end + 4);

    auto status_eol = find_substr(headers, "\r\n");
    StringView status_line = headers.substr(0_byte, status_eol.value_or(headers.length()));
    if (not status_line.starts_with("HTTP/"))
        throw runtime_error("not an http response");
    auto space = find(status_line, ' ');
    if (status_line.end() - space < 4)
        throw runtime_error("malformed http status line");
    StringView code{space + 1, space + 4};
    for (char c : code)
        if (c < '0' or c > '9')
            throw runtime_error("malformed http status code");
    const int status = str_to_int(code);

    Optional<size_t> content_length;
    bool chunked = false;
    StringView remaining = status_eol ? headers.substr(*status_eol + 2) : StringView{};
    while (not remaining.empty())
    {
        auto eol = find_substr(remaining, "\r\n");
        StringView line = remaining.substr(0_byte, eol.value_or(remaining.length()));
        remaining = eol ? remaining.substr(*eol + 2) : StringView{};
        auto colon = find(line, ':');
        if (colon == line.end())
            continue;
        StringView name{line.begin(), colon};
        StringView value{colon + 1, line.end()};
        while (not value.empty() and (value[0_byte] == ' ' or value[0_byte] == '\t'))
            value = value.substr(1_byte);
        if (iequal(name, "content-length"))
        {
            if (value.empty())
                throw runtime_error("malformed content-length");
            size_t len = 0;
            for (char c : value)
            {
                if (c < '0' or c > '9')
                    throw runtime_error("malformed content-length");
                len = len * 10 + (size_t)(c - '0');
            }
            content_length = len;
        }
        else if (iequal(name, "transfer-encoding") and find_substr(value, "chunked"))
            chunked = true;
    }

    if (content_length)
    {
        if ((size_t)(int)body.length() < *content_length)
        {
            if (eof)
                throw runtime_error("truncated http body");
            return {};
        }
        return HttpResponse{status, String{body.substr(0_byte, ByteCount{(int)*content_length})}};
    }
    if (chunked)
    {
        String dechunked;
        while (true)
        {
            auto eol = find_substr(body, "\r\n");
            if (not eol)
            {
                if (eof)
                    throw runtime_error("truncated chunked body");
                return {};
            }
            StringView size_line{body.begin(), body.begin() + (int)*eol};
            size_line = StringView{size_line.begin(), find(size_line, ';')}; // chunk extensions
            if (size_line.empty())
                throw runtime_error("malformed chunk size");
            size_t len = 0;
            for (char c : size_line)
            {
                int v = c >= '0' and c <= '9' ? c - '0'
                      : c >= 'a' and c <= 'f' ? c - 'a' + 10
                      : c >= 'A' and c <= 'F' ? c - 'A' + 10 : -1;
                if (v < 0)
                    throw runtime_error("malformed chunk size");
                len = len * 16 + (size_t)v;
            }
            body = body.substr(*eol + 2);
            if (len == 0)
                return HttpResponse{status, std::move(dechunked)};
            if ((size_t)(int)body.length() < len + 2) // data + trailing CRLF
            {
                if (eof)
                    throw runtime_error("truncated chunked body");
                return {};
            }
            dechunked += body.substr(0_byte, ByteCount{(int)len});
            body = body.substr(ByteCount{(int)len + 2});
        }
    }
    // No framing information: Connection: close, the body ends at EOF.
    if (not eof)
        return {};
    return HttpResponse{status, String{body}};
}

} // anonymous namespace

// One asynchronous POST over a fresh localhost TCP connection, driven by the
// event loop. Terminal states invoke on_done exactly once; on_done may destroy
// this object (the FDWatcher-self-destruction pattern of remote.cc Accepter),
// so nothing runs after it. Destroying the object cancels the request (the
// closed socket makes llama-server abort the generation).
class HttpRequest
{
public:
    using OnDone = Function<void (bool ok, int status, String body)>;

    HttpRequest(const HttpUrl& url, String request, OnDone on_done)
        : m_request{std::move(request)}, m_on_done{std::move(on_done)}
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            throw runtime_error(format("socket call failed, errno: {}", ::strerror(errno)));
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)url.port);
        if (::inet_pton(AF_INET, url.host.c_str(), &addr.sin_addr) != 1)
        {
            ::close(fd);
            throw runtime_error(format("invalid host '{}'", url.host));
        }
        if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0 and errno != EINPROGRESS)
        {
            ::close(fd);
            throw runtime_error(format("connect failed, errno: {}", ::strerror(errno)));
        }
        m_watcher.emplace(fd, FdEvents::Write, EventMode::Urgent,
                          [this](FDWatcher&, FdEvents, EventMode) { step(); });
    }

    ~HttpRequest()
    {
        if (m_watcher and m_watcher->fd() != -1)
            m_watcher->close_fd();
    }

private:
    enum class State { Connecting, Sending, Receiving };

    void step()
    {
        const int fd = m_watcher->fd();
        if (m_state == State::Connecting)
        {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 or err != 0)
            {
                finish(false, 0, {});
                return;
            }
            m_state = State::Sending;
        }
        if (m_state == State::Sending)
        {
            while (m_sent < (int)m_request.length())
            {
                ssize_t n = ::write(fd, m_request.data() + m_sent,
                                    (size_t)((int)m_request.length() - m_sent));
                if (n < 0)
                {
                    if (errno == EAGAIN or errno == EWOULDBLOCK)
                        return; // wait for the next writable event
                    if (errno == EINTR)
                        continue;
                    finish(false, 0, {}); // EPIPE & friends (SIGPIPE is ignored globally)
                    return;
                }
                m_sent += (int)n;
            }
            m_watcher->events() = FdEvents::Read;
            m_state = State::Receiving;
            return;
        }

        bool eof = false;
        while (fd_readable(fd))
        {
            char buffer[4096];
            ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n < 0)
            {
                if (errno == EAGAIN or errno == EWOULDBLOCK)
                    break;
                if (errno == EINTR)
                    continue;
                finish(false, 0, {});
                return;
            }
            if (n == 0)
            {
                eof = true;
                break;
            }
            m_data += StringView{buffer, buffer + n};
        }
        try
        {
            // Engaged as soon as Content-Length is satisfied — no need to
            // wait for the server's FIN.
            if (auto response = parse_http_response(m_data, eof))
            {
                finish(true, response->status, std::move(response->body));
                return;
            }
            if (eof) // defensive; parse throws on truncation at eof
                finish(false, 0, {});
        }
        catch (runtime_error&)
        {
            finish(false, 0, {});
        }
    }

    void finish(bool ok, int status, String body)
    {
        auto done = std::move(m_on_done); // survives destruction of *this
        m_watcher->close_fd();
        done(ok, status, std::move(body));
        // *this may be destroyed at this point; nothing else may run.
    }

    State m_state = State::Connecting;
    String m_request;
    int m_sent = 0;
    String m_data;
    OnDone m_on_done;
    Optional<FDWatcher> m_watcher;
};

// The spawned completion server. Its stdout/stderr must be drained (an unread
// pipe would eventually block llama-server); lines are surfaced to *debug*
// only under fim_debug.
struct FIMServerProcess
{
    explicit FIMServerProcess(Shell s) : shell{std::move(s)} {}

    Shell shell;
    String out_buf, err_buf;
    bool exited = false;
    Optional<FDWatcher> out_watcher, err_watcher;
};

namespace
{

void drain_server_output(FDWatcher& watcher, String& buf, bool& exited)
{
    const int fd = watcher.fd();
    const bool keep = fim_debug_enabled();
    char buffer[1024];
    while (fd_readable(fd))
    {
        ssize_t size = ::read(fd, buffer, sizeof(buffer));
        if (size <= 0)
        {
            if (size < 0 and (errno == EAGAIN or errno == EINTR))
                continue;
            watcher.disable();
            exited = true; // EOF: the server is gone; respawn on the next request
            return;
        }
        if (keep)
            buf += StringView{buffer, buffer + size};
    }
    while (keep)
    {
        auto nl = find_substr(buf, "\n");
        if (not nl)
            break;
        trace(format("server: {}", StringView{buf}.substr(0_byte, *nl)));
        buf = String{StringView{buf}.substr(*nl + 1)};
    }
}

} // anonymous namespace

FIMManager::FIMManager()
    : m_retry_timer{TimePoint::max(), [this](Timer&) { retry(); }}
{
    auto& reg = GlobalScope::instance().option_registry();
    reg.declare_option<String>("fim_cmd",
        "command line spawning a completion server speaking llama.cpp's /infill protocol; "
        "empty disables fim "
        "(e.g. llama-server --port 8012 -hf ggml-org/Qwen2.5-Coder-7B-Q8_0-GGUF -ngl 99 --cache-reuse 256)",
        {});
    reg.declare_option<String>("fim_url",
        "url of the completion server, http://<ipv4|localhost>[:port]",
        "http://127.0.0.1:8012");
    reg.declare_option<RangeAndStringList>("fim_ghost_text",
        "completion preview rendered by the fim ghost-text highlighter (managed by fim)", {});
    reg.declare_option<int>("fim_max_tokens",
        "maximum number of tokens a fim completion may generate", 64);
    reg.declare_option<double>("fim_temperature",
        "sampling temperature for fim completions; 0 is greedy so identical context yields "
        "identical results, higher is more varied (negative defers to the server default)",
        0.0);
    reg.declare_option<int>("fim_context_lines",
        "lines of context sent to the completion server before and after the cursor", 128);
    reg.declare_option<int>("fim_context_bytes",
        "maximum bytes of prefix and suffix context sent to the completion server (0 disables the byte cap)",
        32768);
    reg.declare_option<int>("fim_predict_timeout_ms",
        "maximum milliseconds spent generating a fim completion after the first token (0 disables the timeout)",
        750);
    reg.declare_option<int>("fim_cache_reuse",
        "minimum token chunk size llama-server may reuse from the prompt cache (0 disables request-level cache reuse)",
        256);
    reg.declare_option<int>("fim_slot",
        "llama-server slot used for fim requests to keep prompt cache hot (-1 lets the server choose an idle slot)",
        0);
    reg.declare_option<bool>("fim_debug",
        "log fim http traffic and completion-server output to the *debug* buffer", false);

    auto& cm = CommandManager::instance();
    // Commands may fire from hooks during shutdown, after this singleton is
    // already gone — guard every entry point with has_instance().
    auto cmd = [&](const char* name, const char* doc, void (FIMManager::*method)(Context&)) {
        cm.register_command(name,
            [method](const ParametersParser&, Context& context, const ShellContext&) {
                if (FIMManager::has_instance())
                    (FIMManager::instance().*method)(context);
            }, doc, ParameterDesc{});
    };
    cmd("fim-enable", "enable inline ai completion (this window and windows opened later)", &FIMManager::enable_window);
    cmd("fim-auto-enable", "enable inline ai completion if fim_cmd is set and fim is not disabled", &FIMManager::auto_enable);
    cmd("fim-disable", "disable inline ai completion (stays off for windows opened later)", &FIMManager::disable_window);
    cmd("fim-request", "request an ai completion at the cursor", &FIMManager::request);
    cmd("fim-on-char", "advance the completion preview over typed text", &FIMManager::on_insert_char);
    cmd("fim-clear", "discard the completion preview and any pending request", &FIMManager::clear);
    cmd("fim-accept", "insert the previewed ai completion", &FIMManager::accept);
    cmd("fim-accept-line", "insert the first line of the previewed ai completion", &FIMManager::accept_line);
    cmd("fim-menu-hide", "drop the completion preview if the menu inserted text", &FIMManager::menu_hide);
    cmd("fim-stop", "stop the ai completion server", &FIMManager::stop);

    Context empty{Context::EmptyContextFlag{}};
    auto& hooks = GlobalScope::instance().hooks();
    // WinDisplay in addition to WinCreate so windows that already existed
    // pick fim up when fim_cmd is set mid-session (auto-enable re-runs are
    // idempotent and near-free when unconfigured).
    hooks.add_hook(Hook::WinCreate, "fim", HookFlags::None, Regex{".*"},
                   "fim-auto-enable", empty);
    hooks.add_hook(Hook::WinDisplay, "fim", HookFlags::None, Regex{".*"},
                   "fim-auto-enable", empty);
    hooks.add_hook(Hook::KakEnd, "fim", HookFlags::None, Regex{".*"},
                   "fim-stop", empty);
}

FIMManager::~FIMManager() = default;

void FIMManager::enable_window(Context& context)
{
    m_disabled = false; // explicit enable clears a previous fim-disable
    if (not context.has_window())
        return;
    auto& cm = CommandManager::instance();
    auto run = [&](StringView command) {
        try { cm.execute(command, context); }
        catch (runtime_error&) {} // already set up on this window
    };
    run("add-highlighter window/fim_ghost ghost-text fim_ghost_text");
    run("remove-hooks window fim");
    run("hook -group fim window InsertIdle .* fim-request");
    run("hook -group fim window InsertChar .* fim-on-char");
    run("hook -group fim window InsertDelete .* fim-clear");
    run("hook -group fim window InsertMove .* fim-clear");
    run("hook -group fim window ModeChange pop:insert:.* fim-clear");
    run("hook -group fim window InsertCompletionHide .* fim-menu-hide");
    // Dedicated accept keys — the completion menu owns <tab> for most setups.
    // Never fight an existing insert-mode mapping (any scope).
    if (not context.keymaps().is_mapped(Key{Key::Modifiers::Control, 'f'}, KeymapMode::Insert))
        run("map window insert <c-f> '<a-;>:fim-accept<ret>'");
    if (not context.keymaps().is_mapped(Key{Key::Modifiers::Alt, Key::Tab}, KeymapMode::Insert))
        run("map window insert <a-tab> '<a-;>:fim-accept-line<ret>'");

    if (GlobalScope::instance().options()["fim_cmd"].get<String>().empty())
        debug("fim_cmd is empty; no completions will be requested until it is set");
}

void FIMManager::auto_enable(Context& context)
{
    if (m_disabled) // fim-disable turns auto-enable off for new windows too
        return;
    if (not context.has_buffer() or not context.has_window())
        return;
    if (not (context.buffer().flags() & Buffer::Flags::File))
        return;
    if (GlobalScope::instance().options()["fim_cmd"].get<String>().empty())
        return;
    enable_window(context);
}

void FIMManager::disable_window(Context& context)
{
    m_disabled = true; // stays off for windows opened later, until fim-enable
    if (not context.has_window())
        return;
    auto& cm = CommandManager::instance();
    auto run = [&](StringView command) {
        try { cm.execute(command, context); }
        catch (runtime_error&) {}
    };
    run("remove-hooks window fim");
    run("remove-highlighter window/fim_ghost");
    // The expected-keys argument makes unmap a no-op when the mapping is not
    // ours (enable_window skips mapping over a user's own keys).
    run("unmap window insert <c-f> '<a-;>:fim-accept<ret>'");
    run("unmap window insert <a-tab> '<a-;>:fim-accept-line<ret>'");
    clear(context);
}

bool FIMManager::ghost_intact_at(const Buffer& buffer, BufferCoord cursor) const
{
    return m_ghost and m_ghost->bufname == buffer.name()
       and m_ghost->anchor == cursor and m_ghost->timestamp == buffer.timestamp();
}

void FIMManager::request(Context& context)
{
    if (not context.has_buffer())
        return;
    Buffer& buffer = context.buffer();
    if (context.selections().size() != 1) // multi-cursor completion not supported
        return;
    const BufferCoord cursor = context.selections().main().cursor();
    if (ghost_intact_at(buffer, cursor))
        return; // the current preview is still valid, nothing to ask
    clear_ghost();

    if (GlobalScope::instance().options()["fim_cmd"].get<String>().empty())
    {
        cancel_request();
        return;
    }
    start_server_ifn(context);
    m_retry_count = 0;
    send_infill(buffer, cursor);
}

void FIMManager::start_server_ifn(const Context& context)
{
    if (m_server and not m_server->exited)
        return;
    const bool restart = (bool)m_server;
    m_server.reset();
    if (restart)
        debug("completion server exited, restarting");

    const String& cmd = GlobalScope::instance().options()["fim_cmd"].get<String>();
    try
    {
        m_server = make_unique_ptr<FIMServerProcess>(
            ShellManager::instance().spawn(cmd, context, false));
        FIMServerProcess* sp = m_server.get();
        sp->out_watcher.emplace((int)sp->shell.out, FdEvents::Read, EventMode::Urgent,
            [sp](FDWatcher& w, FdEvents, EventMode) { drain_server_output(w, sp->out_buf, sp->exited); });
        sp->err_watcher.emplace((int)sp->shell.err, FdEvents::Read, EventMode::Urgent,
            [sp](FDWatcher& w, FdEvents, EventMode) { drain_server_output(w, sp->err_buf, sp->exited); });
        trace("started completion server");
    }
    catch (runtime_error& err)
    {
        debug(format("failed to start completion server: {}", err.what()));
    }
}

void FIMManager::send_infill(Buffer& buffer, BufferCoord cursor)
{
    auto& options = GlobalScope::instance().options();
    auto url = parse_http_url(options["fim_url"].get<String>());
    if (not url)
    {
        debug("fim_url must look like http://<ipv4|localhost>[:port]");
        return;
    }
    const LineCount context_lines{options["fim_context_lines"].get<int>()};

    const BufferCoord begin{std::max(0_line, cursor.line - context_lines), 0};
    const BufferCoord end = std::min(buffer.end_coord(),
                                     BufferCoord{cursor.line + context_lines + 1, 0});
    String input_prefix = buffer.string(begin, cursor);
    String input_suffix = buffer.string(cursor, end);
    const int context_bytes = options["fim_context_bytes"].get<int>();
    if (context_bytes > 0)
    {
        input_prefix = trim_suffix_to_bytes(std::move(input_prefix), context_bytes);
        input_suffix = trim_prefix_to_bytes(std::move(input_suffix), context_bytes);
    }

    JsonObject body;
    body.insert({"input_prefix", Value{std::move(input_prefix)}});
    body.insert({"input_suffix", Value{std::move(input_suffix)}});
    body.insert({"n_predict", Value{options["fim_max_tokens"].get<int>()}});
    const double temperature = options["fim_temperature"].get<double>();
    if (temperature >= 0)
        body.insert({"temperature", Value{temperature}});
    body.insert({"stream", Value{false}});
    body.insert({"cache_prompt", Value{true}});
    const int predict_timeout = options["fim_predict_timeout_ms"].get<int>();
    if (predict_timeout > 0)
        body.insert({"t_max_predict_ms", Value{predict_timeout}});
    const int cache_reuse = options["fim_cache_reuse"].get<int>();
    if (cache_reuse > 0)
        body.insert({"n_cache_reuse", Value{cache_reuse}});
    const int slot = options["fim_slot"].get<int>();
    if (slot >= 0)
        body.insert({"id_slot", Value{slot}});

    JsonArray response_fields;
    response_fields.push_back(Value{String{"content"}});
    if (fim_debug_enabled())
    {
        response_fields.push_back(Value{String{"tokens_cached"}});
        response_fields.push_back(Value{String{"tokens_evaluated"}});
        response_fields.push_back(Value{String{"truncated"}});
    }
    body.insert({"response_fields", Value{std::move(response_fields)}});
    // llama-server also accepts n_indent and input_extra (cross-file context);
    // left to future tuning.

    String request = build_http_post(url->host, url->port, "/infill",
                                     to_json(Value{std::move(body)}));

    String bufname = buffer.name();
    const size_t timestamp = buffer.timestamp();
    m_request.reset(); // cancel any in-flight request
    m_pending_request.reset();
    const size_t generation = ++m_request_generation;
    try
    {
        m_request = make_unique_ptr<HttpRequest>(*url, std::move(request),
            [generation, bufname, cursor, timestamp](bool ok, int status, String response_body) {
                if (FIMManager::has_instance())
                    FIMManager::instance().request_finished(generation, bufname, cursor, timestamp,
                                                            ok, status, std::move(response_body));
            });
        m_pending_request = PendingRequest{bufname, timestamp};
        trace(format("requested completion at {}.{}", cursor.line + 1, cursor.column + 1));
    }
    catch (runtime_error& err)
    {
        trace(err.what());
        schedule_retry(std::move(bufname), cursor, timestamp);
    }
}

void FIMManager::request_finished(size_t generation, StringView bufname,
                                  BufferCoord anchor, size_t timestamp,
                                  bool ok, int status, String body)
{
    if (generation != m_request_generation)
        return; // a stale callback from a cancelled request

    // Detach before doing anything re-entrant (setting an option fires
    // BufSetOption hooks); also makes destroying the request from its own
    // callback safe. Destroyed when this scope exits.
    auto request = std::move(m_request);
    m_pending_request.reset();

    if (not ok or status == 503) // connection failed, or the model is still loading
    {
        schedule_retry(String{bufname}, anchor, timestamp);
        return;
    }
    if (status != 200)
    {
        debug(format("completion server returned {}: {}", status,
                     StringView{body}.substr(0_byte, std::min(body.length(), 200_byte))));
        return;
    }
    m_retry_count = 0;

    String content;
    try
    {
        Value json = parse_json(body).value;
        if (const Value* c = find_member(json, "content"); c and c->is_a<String>())
            content = std::move(c->as<String>());
        if (fim_debug_enabled())
        {
            String metrics;
            auto append_metric = [&](StringView name) {
                const Value* v = find_member(json, name);
                if (not v)
                    return;
                String value;
                if (v->is_a<int>())
                    value = format("{}", v->as<int>());
                else if (v->is_a<bool>())
                    value = v->as<bool>() ? String{"true"} : String{"false"};
                if (value.empty())
                    return;
                if (not metrics.empty())
                    metrics += ", ";
                metrics += format("{}={}", name, value);
            };
            append_metric("tokens_cached");
            append_metric("tokens_evaluated");
            append_metric("truncated");
            if (not metrics.empty())
                trace(format("server metrics: {}", metrics));
        }
    }
    catch (runtime_error& err)
    {
        debug(format("failed to parse completion response: {}", err.what()));
        return;
    }
    if (all_of(content, [](char c) { return c == ' ' or c == '\t' or c == '\n' or c == '\r'; }))
        return; // empty or whitespace-only completion

    Buffer* buffer = BufferManager::instance().get_buffer_ifp(bufname);
    if (not buffer or buffer->timestamp() != timestamp)
        return; // buffer gone or edited while the completion was generated

    trace(format("completion: {}", StringView{content}.substr(0_byte, std::min(content.length(), 100_byte))));
    set_ghost(*buffer, {String{bufname}, anchor, std::move(content), timestamp});
}

void FIMManager::set_ghost(Buffer& buffer, Ghost ghost)
{
    RangeAndStringList specs;
    specs.prefix = ghost.timestamp;
    specs.list.push_back({InclusiveBufferRange{ghost.anchor, BufferCoord{-1, -1}}, ghost.text});
    // State first: setting the option fires BufSetOption hooks which may
    // re-enter fim commands.
    m_ghost = std::move(ghost);
    set_buffer_option(buffer, "fim_ghost_text", specs);
}

void FIMManager::cancel_request()
{
    ++m_request_generation;
    m_request.reset();
    m_pending_request.reset();
    m_retry_timer.disable();
    m_retry_count = 0;
}

void FIMManager::clear_ghost()
{
    m_retry_timer.disable();
    if (not m_ghost)
        return;
    String bufname = std::move(m_ghost->bufname);
    m_ghost.reset(); // before the option set; its hooks may re-enter
    if (Buffer* buffer = BufferManager::instance().get_buffer_ifp(bufname))
        set_buffer_option(*buffer, "fim_ghost_text",
                          RangeAndStringList{buffer->timestamp(), {}});
}

void FIMManager::clear(Context&)
{
    cancel_request();
    clear_ghost();
}

void FIMManager::on_insert_char(Context& context)
{
    if (not context.has_buffer())
    {
        cancel_request();
        return;
    }
    Buffer& buffer = context.buffer();
    if (not m_ghost)
    {
        if (m_pending_request and m_pending_request->bufname == context.buffer().name()
            and m_pending_request->timestamp != context.buffer().timestamp())
            cancel_request();
        return;
    }
    if (m_ghost->bufname != buffer.name())
    {
        cancel_request();
        return;
    }
    if (context.selections().size() != 1)
    {
        cancel_request();
        clear_ghost();
        return;
    }
    const BufferCoord cursor = context.selections().main().cursor();
    if (cursor <= m_ghost->anchor)
    {
        cancel_request();
        clear_ghost();
        return;
    }
    // Compare everything inserted since the anchor (the typed character plus
    // whatever indent/brace hooks added — InsertChar fires after all of it)
    // against the ghost head; hook params are not reliable here since pastes
    // fire no InsertChar at all.
    const String typed = buffer.string(m_ghost->anchor, cursor);
    const StringView ghost_text = m_ghost->text;
    if (not ghost_text.starts_with(typed))
    {
        cancel_request();
        clear_ghost(); // diverged; the next idle pause requests a fresh one
        return;
    }
    if (typed.length() == ghost_text.length())
    {
        cancel_request();
        clear_ghost(); // the whole completion was typed out
        return;
    }
    set_ghost(buffer, {std::move(m_ghost->bufname), cursor,
                       String{ghost_text.substr(typed.length())}, buffer.timestamp()});
}

void FIMManager::accept(Context& context) { accept_impl(context, false); }
void FIMManager::accept_line(Context& context) { accept_impl(context, true); }

void FIMManager::accept_impl(Context& context, bool line_only)
{
    if (not context.has_buffer())
        return;
    Buffer& buffer = context.buffer();
    auto& selections = context.selections();

    const bool ghost_ready = m_ghost and m_ghost->bufname == buffer.name()
        and selections.size() == 1
        and selections.main().cursor() == m_ghost->anchor
        and m_ghost->timestamp == buffer.timestamp();

    if (not ghost_ready)
        return;

    String text = m_ghost->text;
    String remainder;
    if (line_only)
    {
        if (auto nl = find(text, '\n'); nl != text.end())
        {
            // Include the newline: this is a raw buffer edit, so no indent
            // hooks run and the ghost's own indentation is preserved;
            // repeated presses walk the completion line by line.
            remainder = String{nl + 1, text.end()};
            text = String{text.begin(), nl + 1};
        }
    }
    const BufferCoord anchor = m_ghost->anchor;
    clear_ghost();
    {
        ScopedEdition edition{context};
        buffer.insert(anchor, text);
        selections.update();
    }
    if (not remainder.empty())
        set_ghost(buffer, {buffer.name(), selections.main().cursor(),
                           std::move(remainder), buffer.timestamp()});
}

// The ghost coexists with the completion menu (it renders in the text, the
// menu is an overlay); but a menu selection edits the buffer without firing
// InsertChar, so when the menu closes drop the preview unless it is still
// exactly valid — the next idle pause requests a fresh one.
void FIMManager::menu_hide(Context& context)
{
    if (not context.has_buffer())
    {
        cancel_request();
        return;
    }
    if (not m_ghost)
    {
        if (m_pending_request and m_pending_request->bufname == context.buffer().name()
            and m_pending_request->timestamp != context.buffer().timestamp())
            cancel_request();
        return;
    }
    if (not ghost_intact_at(context.buffer(), context.selections().main().cursor()))
    {
        cancel_request();
        clear_ghost();
    }
}

void FIMManager::stop(Context&)
{
    cancel_request();
    clear_ghost();
    if (m_server)
    {
        trace("stopping completion server");
        m_server.reset(); // SIGTERM + waitpid via UniquePid
    }
}

void FIMManager::schedule_retry(String bufname, BufferCoord anchor, size_t timestamp)
{
    // InsertIdle fires once per pause, so a request lost to server warm-up
    // (connection refused, 503 while the model loads) would otherwise leave
    // the user waiting for nothing; retry in the background for ~60s.
    constexpr int max_retries = 80;
    if (++m_retry_count > max_retries)
    {
        debug("completion server is not responding, giving up until the next request");
        m_retry_count = 0;
        return;
    }
    m_retry = {std::move(bufname), anchor, timestamp};
    m_retry_timer.set_next_date(Clock::now() + std::chrono::milliseconds{750});
    if (m_retry_count == 1)
        trace("completion server not ready, retrying in the background");
}

void FIMManager::retry()
{
    Buffer* buffer = BufferManager::instance().get_buffer_ifp(m_retry.bufname);
    if (not buffer or buffer->timestamp() != m_retry.timestamp)
        return; // the user moved on; the next idle pause starts fresh
    send_infill(*buffer, m_retry.anchor);
}

UnitTest test_fim_http{[]()
{
    // url parsing
    kak_assert(not parse_http_url("https://127.0.0.1:1"));
    kak_assert(not parse_http_url("127.0.0.1:80"));
    kak_assert(not parse_http_url("http://127.0.0.1:0"));
    kak_assert(not parse_http_url("http://127.0.0.1:"));
    auto url = parse_http_url("http://127.0.0.1:8012");
    kak_assert(url and url->host == "127.0.0.1" and url->port == 8012);
    url = parse_http_url("http://localhost");
    kak_assert(url and url->host == "127.0.0.1" and url->port == 80);
    url = parse_http_url("http://10.0.0.2:80/");
    kak_assert(url and url->host == "10.0.0.2" and url->port == 80);

    // byte caps preserve valid UTF-8 boundaries
    kak_assert(trim_prefix_to_bytes("abcdef", 3) == "abc");
    kak_assert(trim_suffix_to_bytes("abcdef", 3) == "def");
    String e_accent_prefix{StringView{"\xC3\xA9" "abc"}};
    String e_accent_suffix{StringView{"abc" "\xC3\xA9"}};
    kak_assert(trim_prefix_to_bytes(e_accent_prefix, 1) == "");
    kak_assert(trim_prefix_to_bytes(e_accent_prefix, 2) == StringView{"\xC3\xA9"});
    kak_assert(trim_suffix_to_bytes(e_accent_suffix, 1) == "");
    kak_assert(trim_suffix_to_bytes(e_accent_suffix, 2) == StringView{"\xC3\xA9"});

    // content-length framing, case-insensitive headers
    StringView full = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\ncontent-length: 5\r\n\r\nhello";
    auto response = parse_http_response(full, false);
    kak_assert(response and response->status == 200 and response->body == "hello");
    kak_assert(not parse_http_response(full.substr(0_byte, 30_byte), false)); // split header
    kak_assert(not parse_http_response(full.substr(0_byte, full.length() - 2), false)); // split body
    response = parse_http_response("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 4\r\n\r\nload", false);
    kak_assert(response and response->status == 503 and response->body == "load");

    // read-to-eof framing (Connection: close without Content-Length)
    kak_assert(not parse_http_response("HTTP/1.1 200 OK\r\n\r\npartial", false));
    response = parse_http_response("HTTP/1.1 200 OK\r\n\r\npartial", true);
    kak_assert(response and response->body == "partial");

    // chunked framing
    StringView chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                         "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    response = parse_http_response(chunked, false);
    kak_assert(response and response->body == "hello world");
    kak_assert(not parse_http_response(chunked.substr(0_byte, 52_byte), false)); // split chunk

    // malformed input throws
    auto throws = [](StringView data, bool eof) {
        try { parse_http_response(data, eof); }
        catch (runtime_error&) { return true; }
        return false;
    };
    kak_assert(throws("garbage\r\n\r\n", false));
    kak_assert(throws("HTTP/1.1 abc\r\n\r\n", false));
    kak_assert(throws("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", true));
    kak_assert(throws("HTTP/1.1 200", true));

    // request builder round-trip sanity
    String request = build_http_post("127.0.0.1", 8012, "/infill", "{\"a\":1}");
    kak_assert(StringView{request}.starts_with("POST /infill HTTP/1.1\r\n"));
    kak_assert(find_substr(request, "Content-Length: 7\r\n"));
    kak_assert(find_substr(request, "Connection: close\r\n"));
    kak_assert(StringView{request}.substr(ByteCount{(int)request.length() - 7}) == "{\"a\":1}");
}};

}
