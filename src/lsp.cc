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
#include "input_handler.hh"
#include "insert_completer.hh" // CompletionList, CompletionCandidate
#include "option_manager.hh"
#include "parameters_parser.hh"
#include "regex.hh"
#include "scope.hh"
#include "selection.hh"
#include "string_utils.hh"
#include "user_interface.hh" // InfoStyle
#include "utf8.hh"

#include <algorithm>
#include <unistd.h>

namespace Kakoune
{

using ServerCommandMap = HashMap<String, String, MemoryDomain::Options>;

namespace
{

// Always written (errors and other things the user should see).
void debug(StringView msg) { write_to_debug_buffer(format("lsp: {}", msg)); }
// Verbose traffic/info; only written when the lsp_debug option is set
// (lsp_debug_enabled is shared with the transport, declared in lsp_client.hh).
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

String find_root(StringView filename)
{
    auto [dir, name] = split_path(filename);
    String cur = real_path(dir.empty() ? StringView{"."} : dir);
    while (not cur.empty())
    {
        if (file_exists(cur + "/.git") or file_exists(cur + "/compile_commands.json"))
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
    LineCount l = clamp(LineCount{line}, 0_line, buffer.line_count() - 1);
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

// Set a buffer-local option value (the LSP result options are all buffer-scoped).
template<typename T>
void set_buffer_option(Buffer& buffer, StringView name, const T& value)
{
    buffer.options().get_local_option(name).set<T>(value);
}

// The buffer for a path: an already-open one if present, else open/create it.
Buffer* find_or_open_buffer(StringView path)
{
    for (auto& b : BufferManager::instance())
        if (b->filename() == path)
            return b.get();
    return open_or_create_file_buffer(path);
}

bool is_word_char(char c)
{
    return (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or
           (c >= '0' and c <= '9') or c == '_';
}

Vector<StringView> split_lines(StringView text)
{
    Vector<StringView> lines;
    const char* start = text.begin();
    for (const char* p = text.begin(); p != text.end(); ++p)
        if (*p == '\n') { lines.push_back({start, p}); start = p + 1; }
    lines.push_back({start, text.end()});
    return lines;
}

// Apply an array of LSP TextEdits to a buffer. Edits are applied last-first so
// earlier ranges keep their coordinates (LSP guarantees they don't overlap).
void apply_text_edits(Buffer& buffer, const JsonArray& edits)
{
    struct Edit { BufferCoord begin, end; String text; };
    Vector<Edit> list;
    for (auto& e : edits)
    {
        const Value* range = find_member(e, "range"_sv);
        const Value* new_text = find_member(e, "newText"_sv);
        if (not range or not new_text or not new_text->is_a<String>())
            continue;
        const Value* s = find_member(*range, "start"_sv);
        const Value* en = find_member(*range, "end"_sv);
        if (not s or not en)
            continue;
        list.push_back({coord_from_position(buffer, *s),
                        coord_from_position(buffer, *en), new_text->as<String>()});
    }
    std::sort(list.begin(), list.end(),
              [](const Edit& a, const Edit& b) { return b.begin < a.begin; });
    for (auto& e : list)
        if (e.begin <= e.end)
            buffer.replace(e.begin, e.end, e.text); // LSP end is exclusive, like replace()
}

// Apply an LSP WorkspaceEdit (changes / documentChanges) to the affected buffers.
void apply_workspace_edit(const Value& edit)
{
    if (not edit.is_a<JsonObject>())
        return;
    if (const Value* changes = find_member(edit, "changes"_sv); changes and changes->is_a<JsonObject>())
        for (auto& item : changes->as<JsonObject>())
            if (item.value.is_a<JsonArray>())
                if (Buffer* buf = find_or_open_buffer(uri_to_path(item.key)))
                    apply_text_edits(*buf, item.value.as<JsonArray>());
    if (const Value* dchanges = find_member(edit, "documentChanges"_sv); dchanges and dchanges->is_a<JsonArray>())
        for (auto& dc : dchanges->as<JsonArray>())
        {
            const Value* td = find_member(dc, "textDocument"_sv);
            const Value* edits = find_member(dc, "edits"_sv);
            const Value* uri = td ? find_member(*td, "uri"_sv) : nullptr;
            if (uri and uri->is_a<String>() and edits and edits->is_a<JsonArray>())
                if (Buffer* buf = find_or_open_buffer(uri_to_path(uri->as<String>())))
                    apply_text_edits(*buf, edits->as<JsonArray>());
        }
}

// Inline-range face + gutter-marker color for an LSP diagnostic severity.
struct DiagnosticStyle { StringView face, gutter; };
DiagnosticStyle diagnostic_style(int severity)
{
    switch (severity)
    {
        case 1:  return {"DiagnosticError",   "red"};
        case 2:  return {"DiagnosticWarning", "yellow"};
        case 3:  return {"DiagnosticInfo",    "blue"};
        default: return {"DiagnosticHint",    "cyan"};
    }
}

// Map an LSP semantic token (type + modifiers) to a face. Mirrors the user's
// former kak-lsp lsp_semantic_tokens config. Unmapped tokens return empty so
// they keep their syntax-highlighting colors.
StringView semantic_face(StringView type, bool readonly, bool user_defined)
{
    if (type == "variable" and readonly) return "constant";
    if (type == "parameter")             return "parameter";
    if (type == "class")                 return "user_type";
    if (type == "type" and user_defined) return "user_type";
    if (type == "typeParameter")         return "user_type";
    if (type == "property")              return "member_variable";
    if (type == "enum")                  return "user_type";
    if (type == "namespace")             return "user_type";
    if (type == "macro")                 return "constant";
    if (type == "enumMember")            return "constant";
    if (type == "variable")              return "variable";
    if (type == "function")              return "function";
    return {};
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
    run("add-highlighter window/lsp_semantic_tokens ranges lsp_semantic_tokens");
    run("add-highlighter window/lsp_diagnostic_ranges ranges lsp_diagnostic_ranges");
    // -min-width 1 keeps the gutter present even with no diagnostics, so the
    // view does not shift sideways as errors appear and disappear while typing.
    run("add-highlighter window/lsp_diagnostic_lines flag-lines -min-width 1 Default lsp_diagnostic_lines");
    // Completion: the stock option= completer renders lsp_completions, which we
    // fill asynchronously; refresh it on idle. Prepend it so it takes precedence
    // over word completion — the first completer that yields candidates wins, so
    // appended it would lose to word=all as soon as a word prefix is typed.
    InsertCompleterDesc lsp_completer{InsertCompleterDesc::Option, "lsp_completions"_str};
    auto completers = context.options()["completers"].get<InsertCompleterDescList>();
    if (not contains(completers, lsp_completer))
    {
        completers.insert(completers.begin(), lsp_completer);
        context.options().get_local_option("completers").set(completers);
    }
    run("remove-hooks window lsp-completion");
    run("hook -group lsp-completion window InsertIdle .* lsp-complete");
    // Semantic highlighting: refresh when idle in normal mode / on reload.
    run("remove-hooks window lsp-semantic");
    run("hook -group lsp-semantic window NormalIdle .* lsp-semantic-tokens");
    run("hook -group lsp-semantic window BufReload .* lsp-semantic-tokens");
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
    reg.declare_option<RangeAndStringList>("lsp_semantic_tokens",
        "semantic highlighting tokens from the language server", {});
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
    // Curly underline with a colored underline (3rd color), default fg/bg so the
    // squiggle is drawn *over* syntax/semantic colors rather than replacing them.
    add_face("DiagnosticError", "default,default,red+c");
    add_face("DiagnosticWarning", "default,default,yellow+c");
    add_face("DiagnosticInfo", "default,default,blue+c");
    add_face("DiagnosticHint", "default,default,cyan+c");

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
    cmd("lsp-auto-start", "start a language server for the current buffer if one is configured", &LSPManager::auto_start);
    cmd("lsp-stop", "stop the language server handling the current buffer", &LSPManager::stop);
    cmd("lsp-sync", "send pending changes of the current buffer to its language server", &LSPManager::sync);
    cmd("lsp-definition", "jump to the definition of the symbol under the cursor", &LSPManager::definition);
    cmd("lsp-hover", "show information about the symbol under the cursor", &LSPManager::hover);
    cmd("lsp-complete", "request completions at the cursor from the language server", &LSPManager::complete);
    cmd("lsp-semantic-tokens", "request semantic highlighting tokens from the language server", &LSPManager::semantic_tokens);
    cmd("lsp-references", "list references to the symbol under the cursor", &LSPManager::references);
    cmd("lsp-format", "format the current buffer with the language server", &LSPManager::formatting);
    cmd("lsp-code-actions", "show code actions available at the selection", &LSPManager::code_actions);

    cm.register_command("lsp-rename",
        [](const ParametersParser& parser, Context& context, const ShellContext&) {
            if (not LSPManager::has_instance())
                return;
            if (parser.positional_count() > 0)
                LSPManager::instance().rename(context, parser[0]);
            else
                LSPManager::instance().rename_prompt(context);
        }, "rename the symbol under the cursor (prompts for the new name if not given)",
        ParameterDesc{{}, ParameterDesc::Flags::None, 0, 1});

    cm.register_command("lsp-apply-code-action",
        [](const ParametersParser& parser, Context& context, const ShellContext&) {
            if (LSPManager::has_instance() and parser.positional_count() > 0)
                LSPManager::instance().apply_code_action(context, str_to_int(parser[0]));
        }, "apply a code action by index (used by the lsp-code-actions menu)",
        ParameterDesc{{}, ParameterDesc::Flags::None, 1, 1});

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
    // Auto-start: spawn the configured server when a window appears for a
    // buffer (filetype may already be set) or when its filetype changes.
    // lsp-auto-start is silent when lsp_servers has no entry for the language.
    hooks.add_hook(Hook::WinCreate, "lsp", HookFlags::None, Regex{".*"},
                   "lsp-auto-start", empty);
    hooks.add_hook(Hook::WinSetOption, "lsp", HookFlags::None, Regex{"filetype=.*"},
                   "lsp-auto-start", empty);
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
            },
            [](StringView method, const Value& params) -> Value {
                // Servers (e.g. clangd code-action tweaks) ask us to apply edits.
                if (method == "workspace/applyEdit")
                {
                    if (const Value* edit = find_member(params, "edit"_sv))
                        apply_workspace_edit(*edit);
                    JsonObject res;
                    res.insert({"applied", Value{true}});
                    return jobj(std::move(res));
                }
                return {}; // ack other requests with null
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
        text_document.insert({"references", jobj({})});
        text_document.insert({"rename", jobj({})});
        text_document.insert({"codeAction", jobj({})});
        text_document.insert({"formatting", jobj({})});
        {
            JsonArray types;
            for (const char* t : {"namespace","type","class","enum","interface","struct",
                    "typeParameter","parameter","variable","property","enumMember","event",
                    "function","method","macro","keyword","modifier","comment","string",
                    "number","regexp","operator","decorator"})
                types.push_back(Value{String{t}});
            JsonArray mods;
            for (const char* m : {"declaration","definition","readonly","static","deprecated",
                    "abstract","async","modification","documentation","defaultLibrary"})
                mods.push_back(Value{String{m}});
            JsonObject requests; requests.insert({"full", Value{true}});
            JsonArray formats; formats.push_back(Value{String{"relative"}});
            JsonObject st;
            st.insert({"requests", jobj(std::move(requests))});
            st.insert({"tokenTypes", Value{std::move(types)}});
            st.insert({"tokenModifiers", Value{std::move(mods)}});
            st.insert({"formats", Value{std::move(formats)}});
            text_document.insert({"semanticTokens", jobj(std::move(st))});
        }
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
            // Capture the server's semantic-tokens legend (token type names by index).
            if (auto* prov = find_member(sp->capabilities, "semanticTokensProvider"_sv))
                if (auto* legend = find_member(*prov, "legend"_sv))
                {
                    if (auto* types = find_member(*legend, "tokenTypes"_sv); types and types->is_a<JsonArray>())
                        for (auto& t : types->as<JsonArray>())
                            if (t.is_a<String>())
                                sp->semantic_token_types.push_back(t.as<String>());
                    if (auto* mods = find_member(*legend, "tokenModifiers"_sv); mods and mods->is_a<JsonArray>())
                        for (auto& m : mods->as<JsonArray>())
                            if (m.is_a<String>())
                                sp->semantic_token_modifiers.push_back(m.as<String>());
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

void LSPManager::auto_start(Context& context)
{
    if (not context.has_buffer())
        return;
    Buffer& buffer = context.buffer();
    if (not (buffer.flags() & Buffer::Flags::File) or buffer.filename().empty())
        return;
    String language = language_of(buffer);
    if (language.empty())
        return;
    auto& servers = GlobalScope::instance().options()["lsp_servers"].get<ServerCommandMap>();
    if (servers.find(language) == servers.end())
        return; // no server configured for this language: stay quiet
    start(context);
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
    if (auto it = m_diagnostics.find(filename); it != m_diagnostics.end())
        m_diagnostics.remove(it->key);
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
                Buffer* target = find_or_open_buffer(path);
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
    while (word_start.column > 0 and is_word_char(line[word_start.column - 1]))
        word_start.column -= 1;

    // Only auto-complete when there is something to complete against: an
    // identifier prefix under the cursor, or a server trigger character (e.g.
    // '.', '::') right before it. On a blank line or after whitespace this
    // stays quiet, so the menu does not pop open while you are just indenting.
    if (word_start.column == cursor.column)
    {
        bool after_trigger = false;
        if (cursor.column > 0)
        {
            const char prev = line[cursor.column - 1];
            const StringView prev_sv{&prev, &prev + 1};
            const Value* prov = find_member(server->capabilities, "completionProvider"_sv);
            const Value* trig = prov ? find_member(*prov, "triggerCharacters"_sv) : nullptr;
            if (trig and trig->is_a<JsonArray>())
            {
                for (auto& t : trig->as<JsonArray>())
                    if (t.is_a<String>() and t.as<String>() == prev_sv)
                    {
                        after_trigger = true;
                        break;
                    }
            }
            else // server didn't advertise any: the common member-access ones
                after_trigger = (prev == '.' or prev == ':' or prev == '>');
        }
        if (not after_trigger)
            return;
    }

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

            // The menu entry is parsed as display-line markup; escape literal braces.
            auto escape_markup = [](StringView s) {
                String res;
                for (char c : s)
                {
                    if (c == '{')
                        res += '\\';
                    res += c;
                }
                return res;
            };

            struct Entry { String text, label, type, doc; };
            Vector<Entry> entries;
            for (auto& item : *items)
            {
                const Value* label = find_member(item, "label"_sv);
                if (not label or not label->is_a<String>())
                    continue;
                const Value* insert = find_member(item, "insertText"_sv);
                String text = (insert and insert->is_a<String>()) ? insert->as<String>()
                                                                  : label->as<String>();
                String type, doc;
                if (const Value* detail = find_member(item, "detail"_sv);
                    detail and detail->is_a<String>() and not detail->as<String>().empty())
                {
                    doc = detail->as<String>();
                    StringView first_line = doc;
                    if (auto nl = std::find(doc.begin(), doc.end(), '\n'); nl != doc.end())
                        first_line = StringView{doc.begin(), nl};
                    type = first_line.str();
                }
                // documentation: string | MarkupContent{value}
                if (const Value* docs = find_member(item, "documentation"_sv))
                {
                    StringView docs_text;
                    if (docs->is_a<String>())
                        docs_text = docs->as<String>();
                    else if (auto* val = find_member(*docs, "value"_sv); val and val->is_a<String>())
                        docs_text = val->as<String>();
                    if (not docs_text.empty())
                        doc += (doc.empty() ? "" : "\n\n") + docs_text;
                }
                entries.push_back({std::move(text), label->as<String>(),
                                   std::move(type), std::move(doc)});
            }

            // Align the types into a column after the longest label (overlong
            // labels keep their type after a single space rather than pushing
            // the whole column further right).
            constexpr CharCount max_align = 40;
            CharCount label_width = 0;
            for (auto& e : entries)
                if (CharCount len = e.label.char_length(); len <= max_align)
                    label_width = std::max(label_width, len);

            CompletionList completions;
            completions.prefix = format("{}.{}@{}", (int)word_start.line + 1,
                                        (int)word_start.column + 1, timestamp);
            for (auto& e : entries)
            {
                String menu = escape_markup(e.label);
                if (not e.type.empty())
                {
                    CharCount len = e.label.char_length();
                    menu += String{' ', len < label_width ? label_width - len + 1 : 1};
                    menu += "{MenuInfo}" + escape_markup(e.type);
                }
                // Show the full detail/documentation in a panel docked to the
                // menu while the item is selected.
                String on_select = e.doc.empty() ?
                    String{} : "info -style menu -- " + quote(e.doc);
                completions.list.push_back(CompletionCandidate{
                    std::move(e.text), std::move(on_select), std::move(menu)});
            }
            if (completions.list.empty())
                return;

            Buffer* buf = BufferManager::instance().get_buffer_ifp(bufname);
            if (not buf)
                return;
            set_buffer_option(*buf, "lsp_completions", completions);
        });
}

void LSPManager::semantic_tokens(Context& context)
{
    if (not context.has_buffer())
        return;
    Buffer& buffer = context.buffer();
    Server* server = server_for_buffer(buffer);
    if (not server or not server->initialized or server->semantic_token_types.empty())
        return; // server doesn't support semantic tokens
    send_did_change(*server, buffer);

    String bufname = buffer.name();
    Vector<String> legend = server->semantic_token_types; // copy for the callback
    // Resolve the bit positions of the only two modifiers we map, so the decode
    // loop does cheap bit tests instead of per-token string comparisons.
    int readonly_bit = -1, userdef_bit = -1;
    for (int j = 0; j < (int)server->semantic_token_modifiers.size(); ++j)
    {
        if (server->semantic_token_modifiers[j] == "readonly") readonly_bit = j;
        else if (server->semantic_token_modifiers[j] == "userDefined") userdef_bit = j;
    }
    JsonObject td;
    td.insert({"uri", Value{path_to_uri(buffer.filename())}});
    JsonObject params;
    params.insert({"textDocument", jobj(std::move(td))});
    server->client->send_request("textDocument/semanticTokens/full", to_json(jobj(std::move(params))),
        [bufname, legend = std::move(legend), readonly_bit, userdef_bit](Value result, Value error) {
            if (error)
                return;
            const Value* data = find_member(result, "data"_sv);
            if (not data or not data->is_a<JsonArray>())
                return;
            auto& arr = data->as<JsonArray>();
            Buffer* buf = BufferManager::instance().get_buffer_ifp(bufname);
            if (not buf)
                return;

            // data is a flat array of 5-tuples, each delta-encoded against the
            // previous token: deltaLine, deltaStartChar, length, tokenType, modifiers.
            Vector<RangeAndString, MemoryDomain::Options> ranges;
            int line = 0, start = 0;
            for (size_t i = 0; i + 5 <= arr.size(); i += 5)
            {
                if (not (arr[i].is_a<int>() and arr[i+1].is_a<int>() and
                         arr[i+2].is_a<int>() and arr[i+3].is_a<int>()))
                    continue;
                int dline = arr[i].as<int>(), dstart = arr[i+1].as<int>();
                int length = arr[i+2].as<int>(), type = arr[i+3].as<int>();
                int mods = arr[i+4].is_a<int>() ? arr[i+4].as<int>() : 0;
                if (dline != 0) { line += dline; start = dstart; }
                else            { start += dstart; }
                if (length <= 0 or type < 0 or (size_t)type >= legend.size())
                    continue;
                bool readonly = readonly_bit >= 0 and (mods & (1 << readonly_bit));
                bool user_defined = userdef_bit >= 0 and (mods & (1 << userdef_bit));
                StringView face = semantic_face(legend[type], readonly, user_defined);
                if (face.empty())
                    continue;
                if (line < 0 or LineCount{line} >= buf->line_count())
                    continue;
                StringView line_str = (*buf)[LineCount{line}];
                ByteCount b = utf16_to_byte(line_str, start);
                ByteCount e = utf16_to_byte(line_str, start + length);
                if (e <= b)
                    continue;
                ranges.push_back({InclusiveBufferRange{{LineCount{line}, b},
                                                       {LineCount{line}, e - 1}}, face.str()});
            }

            RangeAndStringList list;
            list.prefix = buf->timestamp();
            list.list = std::move(ranges);
            set_buffer_option(*buf, "lsp_semantic_tokens", list);
        });
}

void LSPManager::references(Context& context)
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
    JsonObject td;
    td.insert({"uri", Value{path_to_uri(buffer.filename())}});
    JsonObject ctx;
    ctx.insert({"includeDeclaration", Value{true}});
    JsonObject params;
    params.insert({"textDocument", jobj(std::move(td))});
    params.insert({"position", position_value(buffer, context.selections().main().cursor())});
    params.insert({"context", jobj(std::move(ctx))});
    server->client->send_request("textDocument/references", to_json(jobj(std::move(params))),
        [client_name](Value result, Value error) {
            if (error)
            {
                debug(format("references failed: {}", to_json(error)));
                return;
            }
            if (not result.is_a<JsonArray>() or result.as<JsonArray>().empty())
            {
                debug("no references found");
                return;
            }

            // Collect (path, line, utf16-col), sorted by path then line.
            struct Loc { String path; int line, col; };
            Vector<Loc> locs;
            for (auto& loc : result.as<JsonArray>())
            {
                const Value* uri = find_member(loc, "uri"_sv);
                const Value* range = find_member(loc, "range"_sv);
                const Value* start = range ? find_member(*range, "start"_sv) : nullptr;
                if (not uri or not uri->is_a<String>() or not start)
                    continue;
                int line = 0, col = 0;
                if (auto* l = find_member(*start, "line"_sv); l and l->is_a<int>()) line = l->as<int>();
                if (auto* c = find_member(*start, "character"_sv); c and c->is_a<int>()) col = c->as<int>();
                locs.push_back({uri_to_path(uri->as<String>()), line, col});
            }
            std::sort(locs.begin(), locs.end(), [](const Loc& a, const Loc& b) {
                return a.path != b.path ? a.path < b.path : a.line < b.line;
            });

            // Build grep-style "path:line:col: text" output, reading each file once.
            String content;
            for (size_t i = 0; i < locs.size(); )
            {
                StringView path = locs[i].path;
                Buffer* buf = nullptr;
                for (auto& b : BufferManager::instance())
                    if (b->filename() == path) { buf = b.get(); break; }
                String file_text;
                Vector<StringView> file_lines;
                if (not buf)
                    try { file_text = read_file(path); file_lines = split_lines(file_text); }
                    catch (runtime_error&) {}
                for (; i < locs.size() and locs[i].path == path; ++i)
                {
                    int line = locs[i].line;
                    StringView text = buf ? (line >= 0 and LineCount{line} < buf->line_count() ? (*buf)[LineCount{line}] : StringView{})
                                          : (line >= 0 and line < (int)file_lines.size() ? file_lines[line] : StringView{});
                    while (not text.empty() and (text.back() == '\n' or text.back() == '\r'))
                        text = text.substr(0_byte, text.length() - 1);
                    ByteCount col = text.empty() ? ByteCount{locs[i].col} : utf16_to_byte(text, locs[i].col);
                    content += format("{}:{}:{}: {}\n", path, line + 1, (int)col + 1, text);
                }
            }

            Client* client = ClientManager::instance().get_client_ifp(client_name);
            if (not client)
                return;
            Buffer* refs = BufferManager::instance().get_buffer_ifp("*references*");
            if (refs)
                refs->replace({0, 0}, refs->end_coord(), content);
            else
            {
                refs = create_buffer_from_string("*references*", Buffer::Flags::NoUndo, content);
                refs->options().get_local_option("filetype").set<String>("grep");
            }
            client->context().change_buffer(*refs);
        });
}

void LSPManager::rename(Context& context, StringView new_name)
{
    if (not context.has_buffer() or new_name.empty())
        return;
    Buffer& buffer = context.buffer();
    Server* server = server_for_buffer(buffer);
    if (not server or not server->initialized)
    {
        debug("no initialized language server for this buffer (run lsp-start)");
        return;
    }
    send_did_change(*server, buffer);

    JsonObject td;
    td.insert({"uri", Value{path_to_uri(buffer.filename())}});
    JsonObject params;
    params.insert({"textDocument", jobj(std::move(td))});
    params.insert({"position", position_value(buffer, context.selections().main().cursor())});
    params.insert({"newName", Value{new_name.str()}});
    server->client->send_request("textDocument/rename", to_json(jobj(std::move(params))),
        [](Value result, Value error) {
            if (error)
            {
                debug(format("rename failed: {}", to_json(error)));
                return;
            }
            apply_workspace_edit(result);
        });
}

void LSPManager::rename_prompt(Context& context)
{
    if (not context.has_buffer() or not context.has_client())
        return;
    Buffer& buffer = context.buffer();

    // Prefill the prompt with the identifier under the cursor.
    BufferCoord cursor = context.selections().main().cursor();
    StringView line = cursor.line < buffer.line_count() ? buffer[cursor.line] : StringView{};
    ByteCount b = cursor.column, e = cursor.column;
    while (b > 0 and is_word_char(line[b - 1]))
        b -= 1;
    while (e < line.length() and is_word_char(line[e]))
        e += 1;

    context.input_handler().prompt(
        "rename:", line.substr(b, e - b).str(), {}, context.faces()["Prompt"],
        PromptFlags::None, '_', PromptCompleter{},
        [](StringView name, PromptEvent event, Context& context) {
            if (event != PromptEvent::Validate or name.empty())
                return;
            if (LSPManager::has_instance())
                LSPManager::instance().rename(context, name);
        });
}

void LSPManager::formatting(Context& context)
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

    int tabstop = 8, indentwidth = 4;
    try { tabstop = buffer.options()["tabstop"].get<int>(); } catch (runtime_error&) {}
    try { indentwidth = buffer.options()["indentwidth"].get<int>(); } catch (runtime_error&) {}

    JsonObject options;
    // indentwidth 0 means indent with tabs, of width tabstop.
    options.insert({"tabSize", Value{indentwidth > 0 ? indentwidth : tabstop}});
    options.insert({"insertSpaces", Value{indentwidth > 0}});
    JsonObject td;
    td.insert({"uri", Value{path_to_uri(buffer.filename())}});
    JsonObject params;
    params.insert({"textDocument", jobj(std::move(td))});
    params.insert({"options", jobj(std::move(options))});

    String bufname = buffer.name();
    server->client->send_request("textDocument/formatting", to_json(jobj(std::move(params))),
        [bufname](Value result, Value error) {
            if (error)
            {
                debug(format("formatting failed: {}", to_json(error)));
                return;
            }
            if (not result.is_a<JsonArray>() or result.as<JsonArray>().empty())
                return; // already formatted (or server has no formatter)
            if (Buffer* buf = BufferManager::instance().get_buffer_ifp(bufname))
                apply_text_edits(*buf, result.as<JsonArray>());
        });
}

void LSPManager::code_actions(Context& context)
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

    const Selection& sel = context.selections().main();
    BufferCoord lo = std::min<BufferCoord>(sel.anchor(), sel.cursor());
    BufferCoord hi = std::max<BufferCoord>(sel.anchor(), sel.cursor());
    JsonObject range;
    range.insert({"start", position_value(buffer, lo)});
    range.insert({"end", position_value(buffer, hi)});
    JsonObject td;
    td.insert({"uri", Value{path_to_uri(buffer.filename())}});
    // Pass the published diagnostics overlapping the selection, so the server
    // offers their quick-fixes (e.g. clangd's "add missing include").
    JsonArray diags;
    if (auto it = m_diagnostics.find(buffer.filename()); it != m_diagnostics.end())
        for (auto& d : it->value)
            if (d.begin <= hi and lo <= d.end)
                diags.push_back(parse_json(d.json).value);
    JsonObject ctx;
    ctx.insert({"diagnostics", Value{std::move(diags)}});
    JsonObject params;
    params.insert({"textDocument", jobj(std::move(td))});
    params.insert({"range", jobj(std::move(range))});
    params.insert({"context", jobj(std::move(ctx))});

    String client_name = context.name();
    server->client->send_request("textDocument/codeAction", to_json(jobj(std::move(params))),
        [client_name](Value result, Value error) {
            if (not LSPManager::has_instance())
                return;
            if (error or not result.is_a<JsonArray>() or result.as<JsonArray>().empty())
            {
                debug("no code actions available");
                return;
            }
            Client* client = ClientManager::instance().get_client_ifp(client_name);
            if (not client)
                return;

            auto& actions = result.as<JsonArray>();
            auto& store = LSPManager::instance().m_code_actions;
            store.clear();
            String menu = "menu -auto-single";
            for (auto& action : actions)
            {
                const Value* title = find_member(action, "title"_sv);
                StringView label = title and title->is_a<String>() ? title->as<String>()
                                 : action.is_a<String>() ? action.as<String>() : StringView{"(action)"};
                menu += format(" {} {}", quote(label),
                               quote(format("lsp-apply-code-action {}", store.size())));
                store.push_back(std::move(action));
            }
            try { CommandManager::instance().execute(menu, client->context()); }
            catch (runtime_error& err) { debug(format("code-action menu failed: {}", err.what())); }
        });
}

void LSPManager::apply_code_action(Context& context, int index)
{
    if (index < 0 or index >= (int)m_code_actions.size())
        return;
    const Value& action = m_code_actions[index];
    // CodeAction with an inline edit: apply it directly.
    if (const Value* edit = find_member(action, "edit"_sv))
        apply_workspace_edit(*edit);

    // A command to run: action.command is a string (bare Command) or a Command
    // object (CodeAction.command). Send it via workspace/executeCommand.
    const Value* command = find_member(action, "command"_sv);
    const Value* cmd_id = command;
    const Value* args = find_member(action, "arguments"_sv);
    if (command and command->is_a<JsonObject>())
    {
        cmd_id = find_member(*command, "command"_sv);
        args = find_member(*command, "arguments"_sv);
    }
    if (not cmd_id or not cmd_id->is_a<String>() or not context.has_buffer())
        return;
    Server* server = server_for_buffer(context.buffer());
    if (not server or not server->initialized)
        return;
    JsonObject params;
    params.insert({"command", Value{cmd_id->as<String>()}});
    if (args and args->is_a<JsonArray>())
    {
        JsonArray copy; // Value is move-only; deep-copy the args via a json round-trip
        for (auto& a : args->as<JsonArray>())
            copy.push_back(parse_json(to_json(a)).value);
        params.insert({"arguments", Value{std::move(copy)}});
    }
    server->client->send_request("workspace/executeCommand", to_json(jobj(std::move(params))),
                                 [](Value, Value){});
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
    Vector<StoredDiagnostic> stored;
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
        auto [face, gutter] = diagnostic_style(severity);

        BufferCoord b = coord_from_position(*buffer, *start);
        BufferCoord e = coord_from_position(*buffer, *end);
        stored.push_back({to_json(d), b, e});
        if (e.column > 0)
            e.column -= 1; // LSP end is exclusive; ranges option is inclusive
        else if (e.line > b.line)
            e = b;
        if (e < b)
            e = b;
        ranges.push_back({InclusiveBufferRange{b, e}, face.str()});
        lines.push_back({b.line + 1, "{" + gutter + "}>"});
    }

    RangeAndStringList range_list;
    range_list.prefix = buffer->timestamp();
    range_list.list = std::move(ranges);
    set_buffer_option(*buffer, "lsp_diagnostic_ranges", range_list);

    LineAndSpecList line_list;
    line_list.prefix = buffer->timestamp();
    line_list.list = std::move(lines);
    set_buffer_option(*buffer, "lsp_diagnostic_lines", line_list);

    m_diagnostics[path] = std::move(stored);

    trace(format("published {} diagnostics for {}", range_list.list.size(), buffer->name()));
}

}
