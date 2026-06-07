#include "lsp.hh"

#include "buffer.hh"
#include "buffer_manager.hh"
#include "buffer_utils.hh"
#include "client.hh"
#include "client_manager.hh"
#include "command_manager.hh"
#include "context.hh"
#include "face_registry.hh"
#include "file.hh"
#include "highlighters.hh"   // RangeAndStringList, LineAndSpecList, InclusiveBufferRange
#include "hook_manager.hh"
#include "insert_completer.hh" // CompletionList, CompletionCandidate
#include "option_manager.hh"
#include "parameters_parser.hh"
#include "regex.hh"
#include "scope.hh"
#include "selection.hh"
#include "string_utils.hh"
#include "user_interface.hh" // InfoStyle
#include "utf8.hh"

#include <unistd.h>

namespace Kakoune
{

using ServerCommandMap = HashMap<String, String, MemoryDomain::Options>;

namespace
{

bool lsp_debug_enabled()
{
    try { return GlobalScope::instance().options()["lsp_debug"].get<bool>(); }
    catch (runtime_error&) { return false; }
}

// Always written (errors and other things the user should see).
void debug(StringView msg) { write_to_debug_buffer(format("lsp: {}", msg)); }
// Verbose traffic/info; only written when the lsp_debug option is set.
void trace(StringView msg) { if (lsp_debug_enabled()) write_to_debug_buffer(format("lsp: {}", msg)); }

// ── URI <-> path ────────────────────────────────────────────────────────────

bool is_uri_unreserved(char c)
{
    return (c >= 'A' and c <= 'Z') or (c >= 'a' and c <= 'z') or
           (c >= '0' and c <= '9') or c == '-' or c == '_' or c == '.' or c == '~' or c == '/';
}

String path_to_uri(StringView path)
{
    String res = "file://";
    const char* hex = "0123456789ABCDEF";
    for (char c : path)
    {
        if (is_uri_unreserved(c))
            res += c;
        else
        {
            res += '%';
            res += hex[(unsigned char)c >> 4];
            res += hex[(unsigned char)c & 0xF];
        }
    }
    return res;
}

int hex_value(char c)
{
    if (c >= '0' and c <= '9') return c - '0';
    if (c >= 'a' and c <= 'f') return c - 'a' + 10;
    if (c >= 'A' and c <= 'F') return c - 'A' + 10;
    return -1;
}

String uri_to_path(StringView uri)
{
    StringView rest = uri;
    if (rest.substr(0_byte, 7_byte) == "file://")
        rest = rest.substr(7_byte);
    String res;
    for (auto it = rest.begin(); it != rest.end(); ++it)
    {
        if (*it == '%' and it + 2 < rest.end())
        {
            int hi = hex_value(*(it + 1)), lo = hex_value(*(it + 2));
            if (hi >= 0 and lo >= 0)
            {
                res += (char)(hi * 16 + lo);
                it += 2;
                continue;
            }
        }
        res += *it;
    }
    return res;
}

// ── UTF-8 byte offset <-> UTF-16 code-unit offset (LSP default encoding) ─────

int byte_to_utf16(StringView line, ByteCount byte)
{
    int u16 = 0;
    auto it = line.begin();
    auto target = line.begin() + (int)byte;
    while (it < target and it < line.end())
    {
        Codepoint cp = utf8::read_codepoint(it, line.end());
        u16 += cp > 0xFFFF ? 2 : 1;
    }
    return u16;
}

ByteCount utf16_to_byte(StringView line, int u16_target)
{
    int u16 = 0;
    auto it = line.begin();
    while (it < line.end() and u16 < u16_target)
    {
        Codepoint cp = utf8::read_codepoint(it, line.end());
        u16 += cp > 0xFFFF ? 2 : 1;
    }
    return ByteCount{(int)(it - line.begin())};
}

// ── project root ────────────────────────────────────────────────────────────

bool path_exists(StringView path)
{
    return access(String{path}.c_str(), F_OK) == 0;
}

String find_root(StringView filename)
{
    auto [dir, name] = split_path(filename);
    String cur = real_path(dir.empty() ? StringView{"."} : dir);
    while (not cur.empty())
    {
        if (path_exists(cur + "/.git") or path_exists(cur + "/compile_commands.json"))
            return cur;
        auto [parent, base] = split_path(cur);
        if (parent.empty() or parent == cur)
            break;
        cur = parent.str();
    }
    return real_path(dir.empty() ? StringView{"."} : dir);
}

// ── language detection ──────────────────────────────────────────────────────

String language_of(Buffer& buffer)
{
    auto& reg = GlobalScope::instance().option_registry();
    if (reg.option_exists("filetype"))
    {
        try
        {
            auto& ft = buffer.options()["filetype"].get<String>();
            if (not ft.empty())
                return ft;
        }
        catch (runtime_error&) {}
    }
    // Fall back to the file extension.
    StringView name = buffer.filename();
    auto dot = name.end();
    for (auto it = name.end(); it != name.begin(); )
    {
        --it;
        if (*it == '/') break;
        if (*it == '.') { dot = it; break; }
    }
    if (dot != name.end() and dot + 1 != name.end())
        return StringView{dot + 1, name.end()}.str();
    return {};
}

// ── JSON building helpers ───────────────────────────────────────────────────

Value jobj(JsonObject o) { return Value{std::move(o)}; }

Value position_value(Buffer& buffer, BufferCoord coord)
{
    JsonObject pos;
    StringView line = coord.line < buffer.line_count() ? buffer[coord.line] : StringView{};
    pos.insert({"line", Value{(int)coord.line}});
    pos.insert({"character", Value{byte_to_utf16(line, coord.column)}});
    return jobj(std::move(pos));
}

BufferCoord coord_from_position(Buffer& buffer, const Value& pos)
{
    if (not pos.is_a<JsonObject>())
        return {};
    auto& o = pos.as<JsonObject>();
    int line = 0, character = 0;
    if (auto it = o.find("line"_sv); it != o.end() and it->value.is_a<int>())
        line = it->value.as<int>();
    if (auto it = o.find("character"_sv); it != o.end() and it->value.is_a<int>())
        character = it->value.as<int>();
    LineCount l{line};
    if (l < 0) l = 0;
    if (l >= buffer.line_count()) l = buffer.line_count() - 1;
    if (l < 0) l = 0;
    StringView line_str = buffer[l];
    return buffer.clamp({l, utf16_to_byte(line_str, character)});
}

String text_document_position_params(Buffer& buffer, BufferCoord coord)
{
    JsonObject td;
    td.insert({"uri", Value{path_to_uri(buffer.filename())}});
    JsonObject p;
    p.insert({"textDocument", jobj(std::move(td))});
    p.insert({"position", position_value(buffer, coord)});
    return to_json(jobj(std::move(p)));
}

const Value* find_member(const Value& v, StringView key)
{
    if (not v.is_a<JsonObject>())
        return nullptr;
    auto& o = v.as<JsonObject>();
    auto it = o.find(key);
    return it != o.end() ? &it->value : nullptr;
}

void setup_lsp_window(Context& context)
{
    if (not context.has_window())
        return;
    auto& cm = CommandManager::instance();
    auto run = [&](StringView command) {
        try { cm.execute(command, context); }
        catch (runtime_error&) {} // already set up on this window
    };
    run("add-highlighter window/lsp_diagnostic_ranges ranges lsp_diagnostic_ranges");
    run("add-highlighter window/lsp_diagnostic_lines flag-lines Default lsp_diagnostic_lines");
    // Completion: the stock option= completer renders lsp_completions, which we
    // fill asynchronously; refresh it on idle.
    run("set-option -add window completers option=lsp_completions");
    run("remove-hooks window lsp-completion");
    run("hook -group lsp-completion window InsertIdle .* lsp-complete");
}

}

// ── LSPManager ──────────────────────────────────────────────────────────────

LSPManager::LSPManager()
{
    auto& reg = GlobalScope::instance().option_registry();
    reg.declare_option<ServerCommandMap>("lsp_servers",
        "map of filetype (or file extension) to the language-server command line", {});
    reg.declare_option<RangeAndStringList>("lsp_diagnostic_ranges",
        "diagnostics from the language server, as faces over ranges", {});
    reg.declare_option<LineAndSpecList>("lsp_diagnostic_lines",
        "diagnostic gutter flags from the language server", {});
    reg.declare_option<CompletionList>("lsp_completions",
        "completion candidates from the language server", {});
    reg.declare_option<bool>("lsp_debug",
        "log language-server JSON-RPC traffic and stderr to the *debug* buffer", false);

    // Fallback diagnostic faces for sessions without a colorscheme; a loaded
    // colorscheme (e.g. colors/default.kak) overrides these with override=true.
    auto& faces = GlobalScope::instance().faces();
    auto add_face = [&](StringView name, StringView desc) {
        try { faces.add_face(name, desc, false); } catch (runtime_error&) {}
    };
    add_face("DiagnosticError", "red,default");
    add_face("DiagnosticWarning", "yellow,default");
    add_face("DiagnosticInfo", "blue,default");
    add_face("DiagnosticHint", "cyan,default");

    auto& cm = CommandManager::instance();
    // Commands may fire from hooks during shutdown (e.g. BufClose while the
    // BufferManager tears down), after this singleton is already gone — guard
    // every entry point with has_instance().
    auto cmd = [&](const char* name, const char* doc, void (LSPManager::*method)(Context&)) {
        cm.register_command(name,
            [method](const ParametersParser&, Context& context, const ShellContext&) {
                if (LSPManager::has_instance())
                    (LSPManager::instance().*method)(context);
            }, doc, ParameterDesc{});
    };
    cmd("lsp-start", "start a language server for the current buffer and open it", &LSPManager::start);
    cmd("lsp-stop", "stop the language server handling the current buffer", &LSPManager::stop);
    cmd("lsp-sync", "send pending changes of the current buffer to its language server", &LSPManager::sync);
    cmd("lsp-definition", "jump to the definition of the symbol under the cursor", &LSPManager::definition);
    cmd("lsp-hover", "show information about the symbol under the cursor", &LSPManager::hover);
    cmd("lsp-complete", "request completions at the cursor from the language server", &LSPManager::complete);

    cm.register_command("lsp-did-close",
        [](const ParametersParser& parser, Context&, const ShellContext&) {
            if (LSPManager::has_instance() and parser.positional_count() > 0)
                LSPManager::instance().did_close(parser[0]);
        }, "notify the language server that a buffer was closed",
        ParameterDesc{{}, ParameterDesc::Flags::None, 1, 1});

    cm.register_command("lsp-exit",
        [](const ParametersParser&, Context&, const ShellContext&) {
            if (LSPManager::has_instance())
                LSPManager::instance().exit_all();
        }, "shut down all language servers", ParameterDesc{});

    Context empty{Context::EmptyContextFlag{}};
    auto& hooks = GlobalScope::instance().hooks();
    hooks.add_hook(Hook::BufClose, "lsp", HookFlags::None, Regex{".*"},
                   "lsp-did-close %val{hook_param}", empty);
    hooks.add_hook(Hook::KakEnd, "lsp", HookFlags::None, Regex{".*"},
                   "lsp-exit", empty);
}

LSPManager::~LSPManager() = default;

LSPManager::Server* LSPManager::server_for_buffer(Buffer& buffer)
{
    String language = language_of(buffer);
    if (language.empty())
        return nullptr;
    String key = language + "\x1f" + find_root(buffer.filename());
    for (auto& s : m_servers)
        if (s->key == key)
            return s.get();
    return nullptr;
}

LSPManager::Server* LSPManager::ensure_server(Buffer& buffer, Context& spawn_ctx)
{
    if (auto* existing = server_for_buffer(buffer))
        return existing;

    String language = language_of(buffer);
    if (language.empty())
    {
        debug(format("no language detected for buffer '{}'", buffer.name()));
        return nullptr;
    }

    auto& servers = GlobalScope::instance().options()["lsp_servers"].get<ServerCommandMap>();
    auto cmd_it = servers.find(language);
    if (cmd_it == servers.end())
    {
        debug(format("no lsp_servers entry for language '{}'", language));
        return nullptr;
    }

    String root = find_root(buffer.filename());
    auto srv = make_unique_ptr<Server>();
    srv->key = language + "\x1f" + root;
    srv->language_id = language;
    srv->root = root;
    Server* sp = srv.get();
    try
    {
        srv->client = make_unique_ptr<LSPClient>(cmd_it->value, spawn_ctx,
            [this, sp](StringView method, Value params) {
                on_notification(*sp, method, params);
            });
    }
    catch (runtime_error& err)
    {
        debug(format("failed to spawn '{}': {}", cmd_it->value, err.what()));
        return nullptr;
    }
    m_servers.push_back(std::move(srv));

    // initialize
    JsonObject text_document;
    {
        JsonObject hover; hover.insert({"contentFormat", [] {
            JsonArray a; a.push_back(Value{String{"plaintext"}}); return Value{std::move(a)}; }()});
        text_document.insert({"hover", jobj(std::move(hover))});
        text_document.insert({"definition", jobj({})});
        text_document.insert({"completion", jobj({})});
        text_document.insert({"publishDiagnostics", jobj({})});
        text_document.insert({"synchronization", jobj({})});
    }
    JsonObject caps;
    caps.insert({"textDocument", jobj(std::move(text_document))});

    JsonObject params;
    params.insert({"processId", Value{(int)getpid()}});
    params.insert({"rootUri", Value{path_to_uri(root)}});
    params.insert({"capabilities", jobj(std::move(caps))});

    sp->client->send_request("initialize", to_json(jobj(std::move(params))),
        [sp](Value result, Value error) {
            if (error)
            {
                debug(format("initialize failed: {}", to_json(error)));
                return;
            }
            if (result.is_a<JsonObject>())
            {
                auto& o = result.as<JsonObject>();
                if (auto c = o.find("capabilities"_sv); c != o.end())
                    sp->capabilities = std::move(c->value);
            }
            sp->initialized = true;
            sp->client->send_notification("initialized", "{}");
            for (auto& [method, p] : sp->queued)
                sp->client->send_notification(method, p);
            sp->queued.clear();
            trace(format("server '{}' initialized", sp->language_id));
        });

    return sp;
}

void LSPManager::notify(Server& server, StringView method, String params_json)
{
    if (server.initialized)
        server.client->send_notification(method, std::move(params_json));
    else
        server.queued.push_back({method.str(), std::move(params_json)});
}

void LSPManager::send_did_open(Server& server, Buffer& buffer)
{
    auto& doc = server.docs[buffer.filename()];
    if (doc.open)
        return;
    doc.open = true;
    doc.version = 1;
    doc.synced_timestamp = buffer.timestamp();

    JsonObject td;
    td.insert({"uri", Value{path_to_uri(buffer.filename())}});
    td.insert({"languageId", Value{server.language_id}});
    td.insert({"version", Value{doc.version}});
    td.insert({"text", Value{buffer.string({0, 0}, buffer.end_coord())}});
    JsonObject params;
    params.insert({"textDocument", jobj(std::move(td))});
    notify(server, "textDocument/didOpen", to_json(jobj(std::move(params))));
}

void LSPManager::send_did_change(Server& server, Buffer& buffer)
{
    auto it = server.docs.find(buffer.filename());
    if (it == server.docs.end() or not it->value.open)
        return;
    auto& doc = it->value;
    if (doc.synced_timestamp == buffer.timestamp())
        return; // already up to date

    doc.version += 1;
    doc.synced_timestamp = buffer.timestamp();

    JsonObject td;
    td.insert({"uri", Value{path_to_uri(buffer.filename())}});
    td.insert({"version", Value{doc.version}});
    JsonObject change;
    change.insert({"text", Value{buffer.string({0, 0}, buffer.end_coord())}});
    JsonArray changes;
    changes.push_back(jobj(std::move(change)));
    JsonObject params;
    params.insert({"textDocument", jobj(std::move(td))});
    params.insert({"contentChanges", Value{std::move(changes)}});
    notify(server, "textDocument/didChange", to_json(jobj(std::move(params))));
}

void LSPManager::start(Context& context)
{
    if (not context.has_buffer())
        return;
    Buffer& buffer = context.buffer();
    if (buffer.filename().empty())
    {
        debug("buffer has no filename, cannot start a language server");
        return;
    }
    if (Server* server = ensure_server(buffer, context))
    {
        send_did_open(*server, buffer);
        setup_lsp_window(context);
    }
}

void LSPManager::stop(Context& context)
{
    if (not context.has_buffer())
        return;
    Server* server = server_for_buffer(context.buffer());
    if (not server)
        return;
    if (server->initialized)
    {
        server->client->send_request("shutdown", "null", [](Value, Value){});
        server->client->send_notification("exit", "null");
    }
    for (auto it = m_servers.begin(); it != m_servers.end(); ++it)
        if (it->get() == server)
        {
            m_servers.erase(it);
            break;
        }
}

void LSPManager::sync(Context& context)
{
    if (not context.has_buffer())
        return;
    if (Server* server = server_for_buffer(context.buffer()))
        send_did_change(*server, context.buffer());
}

void LSPManager::did_close(StringView buffer_name)
{
    Buffer* buffer = BufferManager::instance().get_buffer_ifp(buffer_name);
    StringView filename = buffer ? StringView{buffer->filename()} : buffer_name;
    for (auto& server : m_servers)
    {
        auto it = server->docs.find(filename);
        if (it == server->docs.end() or not it->value.open)
            continue;
        JsonObject td;
        td.insert({"uri", Value{path_to_uri(filename)}});
        JsonObject params;
        params.insert({"textDocument", jobj(std::move(td))});
        notify(*server, "textDocument/didClose", to_json(jobj(std::move(params))));
        server->docs.remove(it->key);
    }
}

void LSPManager::exit_all()
{
    for (auto& server : m_servers)
    {
        if (server->initialized and not server->client->is_dead())
        {
            server->client->send_request("shutdown", "null", [](Value, Value){});
            server->client->send_notification("exit", "null");
        }
    }
    m_servers.clear();
}

void LSPManager::definition(Context& context)
{
    if (not context.has_buffer())
        return;
    Buffer& buffer = context.buffer();
    Server* server = server_for_buffer(buffer);
    if (not server or not server->initialized)
    {
        debug("no initialized language server for this buffer (run lsp-start)");
        return;
    }
    send_did_change(*server, buffer);

    String client_name = context.name();
    String params = text_document_position_params(buffer, context.selections().main().cursor());
    server->client->send_request("textDocument/definition", std::move(params),
        [client_name](Value result, Value error) {
            if (error)
            {
                debug(format("definition failed: {}", to_json(error)));
                return;
            }
            // result: Location | Location[] | LocationLink[]
            const Value* loc = nullptr;
            if (result.is_a<JsonObject>())
                loc = &result;
            else if (result.is_a<JsonArray>() and not result.as<JsonArray>().empty())
                loc = &result.as<JsonArray>()[0];
            if (not loc)
            {
                debug("no definition found");
                return;
            }
            const Value* uri = find_member(*loc, "uri"_sv);
            const Value* range = find_member(*loc, "range"_sv);
            if (not uri) uri = find_member(*loc, "targetUri"_sv);
            if (not range) range = find_member(*loc, "targetSelectionRange"_sv);
            if (not uri or not uri->is_a<String>() or not range)
            {
                debug("malformed definition response");
                return;
            }
            const Value* start = find_member(*range, "start"_sv);
            if (not start)
                return;

            Client* client = ClientManager::instance().get_client_ifp(client_name);
            if (not client)
            {
                debug(format("definition: no client named '{}'", client_name));
                return;
            }
            String path = uri_to_path(uri->as<String>());
            try
            {
                Buffer* target = nullptr;
                for (auto& b : BufferManager::instance())
                    if (b->filename() == path) { target = b.get(); break; }
                if (not target)
                    target = open_or_create_file_buffer(path);
                if (not target)
                {
                    debug(format("definition: could not open '{}'", path));
                    return;
                }
                BufferCoord coord = coord_from_position(*target, *start);
                Context& ctx = client->context();
                ctx.push_jump();
                ctx.change_buffer(*target);
                ctx.selections_write_only() = SelectionList{*target, Selection{coord}};
            }
            catch (runtime_error& err)
            {
                debug(format("definition: failed to jump: {}", err.what()));
            }
        });
}

void LSPManager::hover(Context& context)
{
    if (not context.has_buffer() or not context.has_client())
        return;
    Buffer& buffer = context.buffer();
    Server* server = server_for_buffer(buffer);
    if (not server or not server->initialized)
    {
        debug("no initialized language server for this buffer (run lsp-start)");
        return;
    }
    send_did_change(*server, buffer);

    String client_name = context.name();
    BufferCoord anchor = context.selections().main().cursor();
    String params = text_document_position_params(buffer, anchor);
    server->client->send_request("textDocument/hover", std::move(params),
        [client_name, anchor](Value result, Value error) {
            if (error or not result.is_a<JsonObject>())
                return;
            const Value* contents = find_member(result, "contents"_sv);
            if (not contents)
                return;

            // contents: MarkupContent {value} | MarkedString | (MarkedString | string)[]
            String text;
            auto extract = [](const Value& v) -> String {
                if (v.is_a<String>())
                    return v.as<String>();
                if (auto* val = find_member(v, "value"_sv); val and val->is_a<String>())
                    return val->as<String>();
                return {};
            };
            if (contents->is_a<JsonArray>())
            {
                for (auto& part : contents->as<JsonArray>())
                {
                    String s = extract(part);
                    if (not s.empty())
                        text += text.empty() ? s : "\n" + s;
                }
            }
            else
                text = extract(*contents);

            if (text.empty())
                return;
            Client* client = ClientManager::instance().get_client_ifp(client_name);
            if (not client)
                return;
            client->info_show("hover", text, anchor, InfoStyle::Prompt);
        });
}

void LSPManager::complete(Context& context)
{
    if (not context.has_buffer())
        return;
    Buffer& buffer = context.buffer();
    Server* server = server_for_buffer(buffer);
    if (not server or not server->initialized)
        return;
    send_did_change(*server, buffer);

    BufferCoord cursor = context.selections().main().cursor();
    // Anchor completion at the start of the identifier under the cursor so the
    // stock option= completer can rank candidates against the typed prefix.
    BufferCoord word_start = cursor;
    StringView line = cursor.line < buffer.line_count() ? buffer[cursor.line] : StringView{};
    auto is_word = [](char c) {
        return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or
               (c >= '0' and c <= '9') or c == '_';
    };
    while (word_start.column > 0 and is_word(line[word_start.column - 1]))
        word_start.column -= 1;

    String bufname = buffer.name();
    size_t timestamp = buffer.timestamp();
    String params = text_document_position_params(buffer, cursor);
    server->client->send_request("textDocument/completion", std::move(params),
        [bufname, word_start, timestamp](Value result, Value error) {
            if (error)
                return;
            // result: CompletionList { items: [] } | CompletionItem[]
            const JsonArray* items = nullptr;
            if (auto* i = find_member(result, "items"_sv); i and i->is_a<JsonArray>())
                items = &i->as<JsonArray>();
            else if (result.is_a<JsonArray>())
                items = &result.as<JsonArray>();
            if (not items)
                return;

            CompletionList completions;
            completions.prefix = format("{}.{}@{}", (int)word_start.line + 1,
                                        (int)word_start.column + 1, timestamp);
            for (auto& item : *items)
            {
                const Value* label = find_member(item, "label"_sv);
                if (not label or not label->is_a<String>())
                    continue;
                const Value* insert = find_member(item, "insertText"_sv);
                String text = (insert and insert->is_a<String>()) ? insert->as<String>()
                                                                  : label->as<String>();
                completions.list.push_back(CompletionCandidate{
                    std::move(text), String{}, label->as<String>()});
            }
            if (completions.list.empty())
                return;

            Buffer* buf = BufferManager::instance().get_buffer_ifp(bufname);
            if (not buf)
                return;
            buf->options().get_local_option("lsp_completions").set<CompletionList>(completions);
        });
}

void LSPManager::on_notification(Server& server, StringView method, const Value& params)
{
    if (method == "textDocument/publishDiagnostics")
        publish_diagnostics(params);
    else if (method == "window/logMessage" or method == "window/showMessage")
    {
        if (auto* msg = find_member(params, "message"_sv); msg and msg->is_a<String>())
            trace(format("{}: {}", method, msg->as<String>()));
    }
    // other notifications ignored for M1
}

void LSPManager::publish_diagnostics(const Value& params)
{
    const Value* uri = find_member(params, "uri"_sv);
    const Value* diags = find_member(params, "diagnostics"_sv);
    if (not uri or not uri->is_a<String>() or not diags or not diags->is_a<JsonArray>())
        return;

    String path = uri_to_path(uri->as<String>());
    Buffer* buffer = nullptr;
    for (auto& b : BufferManager::instance())
        if (b->filename() == path)
        {
            buffer = b.get();
            break;
        }
    if (not buffer)
        return;

    Vector<RangeAndString, MemoryDomain::Options> ranges;
    Vector<LineAndSpec, MemoryDomain::Options> lines;
    for (auto& d : diags->as<JsonArray>())
    {
        const Value* range = find_member(d, "range"_sv);
        if (not range)
            continue;
        const Value* start = find_member(*range, "start"_sv);
        const Value* end = find_member(*range, "end"_sv);
        if (not start or not end)
            continue;

        int severity = 1;
        if (auto* s = find_member(d, "severity"_sv); s and s->is_a<int>())
            severity = s->as<int>();
        StringView face = severity == 1 ? "DiagnosticError"
                        : severity == 2 ? "DiagnosticWarning"
                        : severity == 3 ? "DiagnosticInfo"
                                        : "DiagnosticHint";

        BufferCoord b = coord_from_position(*buffer, *start);
        BufferCoord e = coord_from_position(*buffer, *end);
        if (e.column > 0)
            e.column -= 1; // LSP end is exclusive; ranges option is inclusive
        else if (e.line > b.line)
            e = b;
        if (e < b)
            e = b;
        ranges.push_back({InclusiveBufferRange{b, e}, face.str()});
        lines.push_back({b.line + 1, "{" + face + "}>"});
    }

    RangeAndStringList range_list;
    range_list.prefix = buffer->timestamp();
    range_list.list = std::move(ranges);
    buffer->options().get_local_option("lsp_diagnostic_ranges").set<RangeAndStringList>(range_list);

    LineAndSpecList line_list;
    line_list.prefix = buffer->timestamp();
    line_list.list = std::move(lines);
    buffer->options().get_local_option("lsp_diagnostic_lines").set<LineAndSpecList>(line_list);

    trace(format("published {} diagnostics for {}", range_list.list.size(), buffer->name()));
}

}
