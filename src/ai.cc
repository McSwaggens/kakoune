#include "ai.hh"

#include "buffer.hh"
#include "buffer_manager.hh"
#include "buffer_utils.hh"
#include "client.hh"
#include "client_manager.hh"
#include "command_manager.hh"
#include "context.hh"
#include "event_manager.hh"
#include "face_registry.hh"
#include "file.hh"
#include "hook_manager.hh"
#include "input_handler.hh"
#include "json.hh"
#include "option_manager.hh"
#include "parameters_parser.hh"
#include "regex.hh"
#include "scope.hh"
#include "shell_manager.hh"
#include "string_utils.hh"
#include "unit_tests.hh"

#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace Kakoune
{

namespace
{

bool ai_debug_enabled()
{
    try { return GlobalScope::instance().options()["ai_debug"].get<bool>(); }
    catch (runtime_error&) { return false; }
}

// Always written (errors the user should see).
void debug(StringView msg) { write_to_debug_buffer(format("ai: {}", msg)); }
// Verbose; only when ai_debug is set.
void trace(StringView msg) { if (ai_debug_enabled()) write_to_debug_buffer(format("ai: {}", msg)); }

const Value* find_member(const Value& v, StringView key)
{
    if (not v.is_a<JsonObject>())
        return nullptr;
    auto& o = v.as<JsonObject>();
    auto it = o.find(key);
    return it != o.end() ? &it->value : nullptr;
}

enum class Provider { Codex, Claude };

Optional<Provider> parse_provider(StringView s)
{
    if (s == "codex") return Provider::Codex;
    if (s == "claude") return Provider::Claude;
    return {};
}

// The project root: nearest ancestor with .git/compile_commands.json, else the
// buffer's directory. (Same heuristic as the LSP client's file-local find_root.)
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

// The shell command for one request. The prompt is fed on stdin, so no user
// text ever reaches argv (no quoting/injection concern); only our own paths do,
// and those are quoted. The agent runs read-only — we apply the result.
String build_command(Provider provider, StringView codex_cmd, StringView claude_cmd,
                     StringView model, StringView root, StringView outfile)
{
    if (provider == Provider::Codex)
    {
        String cmd = format("{} exec -s read-only --skip-git-repo-check -C {} -o {}",
                            codex_cmd, quote(root), quote(outfile));
        if (not model.empty())
            cmd += format(" -m {}", quote(model));
        cmd += " -"; // read the prompt from stdin
        return cmd;
    }
    // Disallow file-editing/exec tools so claude only answers (we apply the
    // text ourselves); not --permission-mode plan, which makes it present a
    // plan for approval and errors in -p mode.
    String cmd = format("{} -p --output-format json "
                        "--disallowedTools Edit,Write,MultiEdit,NotebookEdit,Bash", claude_cmd);
    if (not model.empty())
        cmd += format(" --model {}", quote(model));
    return cmd; // claude -p reads the prompt from stdin
}

String build_prompt(StringView filename, StringView filetype,
                    StringView instruction, StringView selection)
{
    return format("Rewrite the following selection from {} (filetype {}) according to the "
                  "instruction.\nOutput ONLY the replacement text for the selection — no "
                  "explanation, no markdown code fences.\nInstruction: {}\n"
                  "--- SELECTION ---\n{}",
                  filename.empty() ? StringView{"a buffer"} : filename,
                  filetype.empty() ? StringView{"unknown"} : filetype,
                  instruction, selection);
}

// claude -p --output-format json emits an array of streaming events whose
// final {"type":"result","result":"…","is_error":bool} carries the answer
// (older builds emit that object directly — handle both).
String extract_claude_result(StringView out)
{
    try
    {
        Value json = parse_json(out).value;
        const Value* result = nullptr;
        if (json.is_a<JsonArray>())
        {
            for (auto& event : json.as<JsonArray>())
                if (const Value* type = find_member(event, "type");
                    type and type->is_a<String>() and type->as<String>() == "result")
                    result = &event;
        }
        else if (json.is_a<JsonObject>())
            result = &json;
        if (not result)
            return {};
        if (const Value* err = find_member(*result, "is_error");
            err and err->is_a<bool>() and err->as<bool>())
            return {};
        if (const Value* r = find_member(*result, "result"); r and r->is_a<String>())
            return r->as<String>();
    }
    catch (runtime_error&) {}
    return {};
}

// Agents sometimes wrap output in a ``` fence despite instructions. If the
// whole (trimmed) text is one fenced block, return its contents; otherwise the
// text as-is, minus a single trailing newline.
String unwrap_fenced_block(StringView text)
{
    auto is_ws = [](char c) { return c == ' ' or c == '\t' or c == '\n' or c == '\r'; };
    const char* b = text.begin();
    const char* e = text.end();
    while (b != e and is_ws(*b)) ++b;
    while (e != b and is_ws(e[-1])) --e;
    if (e - b >= 6 and StringView{b, b + 3} == "```" and StringView{e - 3, e} == "```")
    {
        const char* inner = b + 3;
        while (inner != e and *inner != '\n') ++inner; // skip the ```lang line
        if (inner != e) ++inner;                       // past the newline
        const char* iend = e - 3;
        if (iend != inner and iend[-1] == '\n') --iend; // newline before the closing fence
        if (inner <= iend)
            return String{StringView{inner, iend}};
    }
    const char* te = text.end();
    if (te != text.begin() and te[-1] == '\n') --te;
    return String{StringView{text.begin(), te}};
}

} // anonymous namespace

// One agent invocation, driven by the event loop: feed the prompt on stdin,
// drain stdout/stderr, and on stdout EOF extract the result and call on_done.
// finish() moves on_done to a local before calling it, so on_done may destroy
// this object (the FDWatcher-self-destruction pattern); nothing runs after.
// Destroying the object cancels the run (SIGTERM via the Shell's UniquePid).
class AIRequest
{
public:
    using OnDone = Function<void (bool ok, String text)>;

    AIRequest(const Context& context, StringView cmdline, bool json_result,
              String outfile, String prompt, OnDone on_done)
        : m_json_result{json_result}, m_outfile{std::move(outfile)},
          m_prompt{std::move(prompt)}, m_on_done{std::move(on_done)},
          m_shell{ShellManager::instance().spawn(cmdline, context, true)}
    {
        auto nonblock = [](int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK); };
        nonblock((int)m_shell.in);
        nonblock((int)m_shell.out);
        nonblock((int)m_shell.err);

        m_in_watcher.emplace((int)m_shell.in, FdEvents::Write, EventMode::Urgent,
            [this](FDWatcher&, FdEvents, EventMode) { on_writable(); });
        m_out_watcher.emplace((int)m_shell.out, FdEvents::Read, EventMode::Urgent,
            [this](FDWatcher&, FdEvents, EventMode) { on_readable(); });
        m_err_watcher.emplace((int)m_shell.err, FdEvents::Read, EventMode::Urgent,
            [this](FDWatcher&, FdEvents, EventMode) { on_err(); });

        int timeout = 0;
        try { timeout = GlobalScope::instance().options()["ai_timeout"].get<int>(); }
        catch (runtime_error&) {}
        if (timeout > 0)
            m_timeout.emplace(Clock::now() + std::chrono::seconds{timeout},
                              [this](Timer&) { finish(false, {}); }, EventMode::Normal);
    }

    ~AIRequest()
    {
        if (not m_outfile.empty())
            ::unlink(m_outfile.c_str());
        // Watchers hold raw fds owned by m_shell; they only unregister here.
        // m_shell's UniquePid SIGTERMs + reaps the agent.
    }

private:
    void on_writable()
    {
        const int fd = (int)m_shell.in;
        while (m_sent < (int)m_prompt.length())
        {
            ssize_t n = ::write(fd, m_prompt.data() + m_sent,
                                (size_t)((int)m_prompt.length() - m_sent));
            if (n < 0)
            {
                if (errno == EAGAIN or errno == EWOULDBLOCK)
                    return; // wait for the next writable event
                if (errno == EINTR)
                    continue;
                break; // EPIPE etc: the agent died; stdout EOF will complete us
            }
            m_sent += (int)n;
        }
        // Prompt fully written: close stdin so the agent sees end-of-input.
        m_in_watcher->disable();
        m_shell.in.close();
    }

    void on_readable()
    {
        const int fd = (int)m_shell.out;
        while (fd_readable(fd))
        {
            char buffer[4096];
            ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n < 0)
            {
                if (errno == EAGAIN or errno == EWOULDBLOCK)
                    return;
                if (errno == EINTR)
                    continue;
                complete(); // treat a read error as end-of-output
                return;
            }
            if (n == 0)
            {
                complete();
                return; // *this may be destroyed
            }
            if (m_json_result) // claude: stdout IS the result; codex narrates, ignore
                m_out += StringView{buffer, buffer + n};
        }
    }

    void on_err()
    {
        const int fd = (int)m_shell.err;
        while (fd_readable(fd))
        {
            char buffer[1024];
            ssize_t n = ::read(fd, buffer, sizeof(buffer));
            if (n < 0)
            {
                if (errno == EAGAIN or errno == EWOULDBLOCK)
                    return;
                if (errno == EINTR)
                    continue;
                m_err_watcher->disable();
                return;
            }
            if (n == 0)
            {
                m_err_watcher->disable();
                return;
            }
            if (m_err.length() < 4096_byte)
                m_err += StringView{buffer, buffer + n};
        }
    }

    void complete()
    {
        String text;
        if (m_json_result)
            text = extract_claude_result(m_out);
        else if (not m_outfile.empty())
            try { text = read_file(m_outfile); } catch (runtime_error&) {}
        text = unwrap_fenced_block(text);
        const bool ok = not text.empty();
        if (not ok and not m_err.empty())
            trace(format("agent stderr: {}", m_err));
        finish(ok, std::move(text));
    }

    void finish(bool ok, String text)
    {
        if (not m_on_done)
            return; // guard the timeout/EOF race
        auto done = std::move(m_on_done);
        if (m_timeout)
            m_timeout->disable();
        done(ok, std::move(text)); // may destroy *this; nothing after
    }

    bool m_json_result;
    String m_outfile;
    String m_prompt;
    int m_sent = 0;
    String m_out;
    String m_err;
    OnDone m_on_done;
    Shell m_shell;
    Optional<FDWatcher> m_in_watcher, m_out_watcher, m_err_watcher;
    Optional<Timer> m_timeout;
};

AIManager::AIManager()
{
    auto& reg = GlobalScope::instance().option_registry();
    reg.declare_option<String>("ai_provider",
        "ai backend for the :ai command: codex or claude", "codex");
    reg.declare_option<String>("ai_codex_command",
        "command used to invoke the codex CLI", "codex");
    reg.declare_option<String>("ai_claude_command",
        "command used to invoke the claude CLI", "claude");
    reg.declare_option<String>("ai_model",
        "model the ai backend should use (empty = its default)", "");
    reg.declare_option<int>("ai_max_parallel",
        "maximum number of agent requests to run at once for a multi-selection :ai", 4);
    reg.declare_option<int>("ai_timeout",
        "seconds before an ai request is abandoned (0 = no timeout)", 0);
    reg.declare_option<bool>("ai_debug",
        "log ai requests and agent output to the *debug* buffer", false);

    auto& cm = CommandManager::instance();
    cm.register_command("ai",
        [](const ParametersParser& parser, Context& context, const ShellContext&) {
            if (not AIManager::has_instance())
                return;
            if (parser.positional_count() > 0)
            {
                String instruction;
                for (size_t i = 0; i < parser.positional_count(); ++i)
                {
                    if (i)
                        instruction += " ";
                    instruction += parser[i];
                }
                AIManager::instance().rewrite(context, instruction);
            }
            else
                AIManager::instance().rewrite_prompt(context);
        }, "rewrite the selection(s) with an ai agent (prompts for the instruction if none given)",
        ParameterDesc{});

    cm.register_command("ai-cancel",
        [](const ParametersParser&, Context& context, const ShellContext&) {
            if (AIManager::has_instance())
                AIManager::instance().cancel(context);
        }, "cancel the running ai request", ParameterDesc{});

    Context empty{Context::EmptyContextFlag{}};
    GlobalScope::instance().hooks().add_hook(Hook::KakEnd, "ai", HookFlags::None,
                                             Regex{".*"}, "ai-cancel", empty);
}

AIManager::~AIManager() = default;

void AIManager::rewrite(Context& context, StringView instruction)
{
    launch(context, instruction);
}

void AIManager::rewrite_prompt(Context& context)
{
    if (not context.has_buffer() or not context.has_client() or not context.has_input_handler())
        return;
    context.input_handler().prompt(
        "ai: ", {}, {}, context.faces()["Prompt"],
        PromptFlags::DropHistoryEntriesWithBlankPrefix, '_', PromptCompleter{},
        [selection_edition = ScopedSelectionEdition{context}]
        (StringView instruction, PromptEvent event, Context& context) {
            if (event != PromptEvent::Validate or instruction.empty())
                return;
            if (AIManager::has_instance())
                AIManager::instance().launch(context, instruction);
        });
}

void AIManager::launch(Context& context, StringView instruction)
{
    if (not context.has_buffer() or not context.has_client() or instruction.empty())
        return;
    Buffer& buffer = context.buffer();
    if (context.selections().size() == 0)
        return;

    auto& options = GlobalScope::instance().options();
    auto provider = parse_provider(options["ai_provider"].get<String>());
    if (not provider)
    {
        debug(format("unknown ai_provider '{}' (use codex or claude)",
                     options["ai_provider"].get<String>()));
        return;
    }

    if (m_edit) // one batch at a time
        cancel_all();

    auto edit = make_unique_ptr<Edit>();
    edit->generation = ++m_generation;
    edit->client_name = context.name();
    edit->buffer_name = buffer.name();
    edit->timestamp = buffer.timestamp();
    edit->json_result = (*provider == Provider::Claude);

    const String codex_cmd = options["ai_codex_command"].get<String>();
    const String claude_cmd = options["ai_claude_command"].get<String>();
    const String model = options["ai_model"].get<String>();
    const String filename = buffer.display_name();
    String filetype;
    try { filetype = context.options()["filetype"].get<String>(); }
    catch (runtime_error&) {}
    const String root = find_root(buffer.filename());

    for (auto& sel : context.selections())
    {
        edit->selections.push_back(sel);
        edit->originals.push_back(content(buffer, sel));
    }
    const size_t count = edit->selections.size();
    edit->results.resize(count);
    edit->active.resize(count);
    edit->outstanding = count;

    for (size_t i = 0; i < count; ++i)
    {
        String outfile;
        if (*provider == Provider::Codex)
        {
            char path[PATH_MAX];
            int fd = open_temp_file(format("{}/kak-ai", tmpdir()), path);
            if (fd >= 0)
            {
                ::close(fd); // codex truncates and writes this path itself
                outfile = String{path};
            }
        }
        edit->outfiles.push_back(outfile);
        edit->commands.push_back(build_command(*provider, codex_cmd, claude_cmd, model, root, outfile));
        edit->prompts.push_back(build_prompt(filename, filetype, instruction, edit->originals[i]));
    }

    m_edit = std::move(edit);
    set_status(m_edit->client_name, format("ai: thinking… (0/{})", count));
    start_next(*m_edit);
}

void AIManager::start_next(Edit& edit)
{
    int cap = 4;
    try { cap = GlobalScope::instance().options()["ai_max_parallel"].get<int>(); }
    catch (runtime_error&) {}
    cap = std::max(1, cap);

    while (edit.running < (size_t)cap and edit.next_index < edit.selections.size())
    {
        Client* client = ClientManager::instance().get_client_ifp(edit.client_name);
        if (not client)
        {
            cancel_all(); // the client is gone; abandon the batch
            return;
        }
        const size_t i = edit.next_index++;
        const size_t gen = edit.generation;
        try
        {
            edit.active[i] = make_unique_ptr<AIRequest>(
                client->context(), edit.commands[i], edit.json_result,
                edit.outfiles[i], edit.prompts[i],
                [gen, i](bool ok, String text) {
                    if (AIManager::has_instance())
                        AIManager::instance().request_done(gen, i, ok, std::move(text));
                });
            ++edit.running;
        }
        catch (runtime_error& err)
        {
            trace(format("spawn failed: {}", err.what()));
            edit.results[i] = {}; // failure; keep going
            if (--edit.outstanding == 0)
            {
                auto owned = std::move(m_edit);
                finish_edit(*owned);
                return;
            }
        }
    }
}

void AIManager::request_done(size_t generation, size_t index, bool ok, String text)
{
    if (not m_edit or m_edit->generation != generation)
        return; // a stale callback from a cancelled batch
    Edit& edit = *m_edit;
    edit.results[index] = ok ? Optional<String>{std::move(text)} : Optional<String>{};
    edit.active[index].reset(); // destroy the finished request (safe self-destruct)
    --edit.running;
    --edit.outstanding;
    const size_t done = edit.selections.size() - edit.outstanding;
    set_status(edit.client_name, format("ai: thinking… ({}/{})", done, edit.selections.size()));
    if (edit.outstanding == 0)
    {
        auto owned = std::move(m_edit); // detach; finish_edit must not touch m_edit
        finish_edit(*owned);
    }
    else
        start_next(edit);
}

void AIManager::finish_edit(Edit& edit)
{
    Buffer* buffer = BufferManager::instance().get_buffer_ifp(edit.buffer_name);
    if (not buffer)
    {
        set_status(edit.client_name, "ai: buffer closed, discarded");
        return;
    }
    if (buffer->timestamp() != edit.timestamp)
    {
        set_status(edit.client_name, "ai: buffer changed during generation, discarded");
        return;
    }

    int ok = 0, fail = 0;
    Vector<String> strings;
    strings.reserve(edit.selections.size());
    for (size_t i = 0; i < edit.selections.size(); ++i)
    {
        if (edit.results[i])
        {
            strings.push_back(*edit.results[i]);
            ++ok;
        }
        else
        {
            strings.push_back(edit.originals[i]); // no-op replace keeps index alignment
            ++fail;
        }
    }
    if (ok == 0)
    {
        set_status(edit.client_name, "ai: no completions produced");
        return;
    }

    Client* client = ClientManager::instance().get_client_ifp(edit.client_name);
    if (not client or not client->context().has_buffer()
        or &client->context().buffer() != buffer)
    {
        set_status(edit.client_name, "ai: client moved, discarded");
        return;
    }
    Context& context = client->context();
    try { buffer->throw_if_read_only(); }
    catch (runtime_error&)
    {
        set_status(edit.client_name, "ai: buffer is read-only");
        return;
    }

    {
        ScopedEdition edition{context};
        SelectionList sels{*buffer, edit.selections};
        sels.replace(strings); // remaps coords as it goes; results end up selected
        context.selections_write_only() = std::move(sels);
    }

    String message = format("ai: rewrote {} selection{}", ok, ok == 1 ? "" : "s");
    if (fail)
        message += format(" ({} failed)", fail);
    set_status(edit.client_name, std::move(message));
}

void AIManager::set_status(StringView client_name, String message)
{
    if (Client* client = ClientManager::instance().get_client_ifp(client_name))
        client->context().print_status({std::move(message),
                                        client->context().faces()["Information"]});
}

void AIManager::cancel(Context& context)
{
    if (not m_edit)
        return;
    String client_name = m_edit->client_name;
    cancel_all();
    set_status(client_name, "ai: cancelled");
}

void AIManager::cancel_all()
{
    ++m_generation;  // drop any in-flight callbacks
    m_edit.reset();  // destroying active requests SIGTERMs their agents
}

UnitTest test_ai{[]() {
    // build_command per provider
    String codex = build_command(Provider::Codex, "codex", "claude", "", "/proj", "/tmp/o");
    kak_assert(find(codex, '-') != codex.end());
    kak_assert(StringView{codex}.starts_with("codex exec"));
    auto contains = [](StringView h, StringView n) {
        return std::search(h.begin(), h.end(), n.begin(), n.end()) != h.end();
    };
    kak_assert(contains(codex, "-s read-only"));
    kak_assert(contains(codex, "--skip-git-repo-check"));
    kak_assert(contains(codex, "-C '/proj'"));
    kak_assert(contains(codex, "-o '/tmp/o'"));
    kak_assert(not contains(codex, "-m "));
    String codex_m = build_command(Provider::Codex, "codex", "claude", "o3", "/p", "/o");
    kak_assert(contains(codex_m, "-m 'o3'"));
    String claude = build_command(Provider::Claude, "codex", "claude", "", "/p", "");
    kak_assert(contains(claude, "claude -p"));
    kak_assert(contains(claude, "--output-format json"));
    kak_assert(contains(claude, "--disallowedTools"));
    kak_assert(not contains(claude, "--permission-mode plan"));
    kak_assert(contains(build_command(Provider::Claude, "c", "claude", "fable", "/p", ""), "--model 'fable'"));

    // claude json result extraction — array form (the real -p shape) + legacy object
    kak_assert(extract_claude_result(R"([{"type":"system"},{"type":"result","result":"foo","is_error":false}])") == "foo");
    kak_assert(extract_claude_result(R"([{"type":"result","result":"err","is_error":true}])") == "");
    kak_assert(extract_claude_result(R"({"type":"result","result":"bar","is_error":false})") == "bar");
    kak_assert(extract_claude_result(R"({"result":"x","is_error":true})") == "");
    kak_assert(extract_claude_result(R"([{"type":"system"}])") == "");
    kak_assert(extract_claude_result("not json") == "");

    // fenced-block unwrapping
    kak_assert(unwrap_fenced_block("int x = 1;") == "int x = 1;");
    kak_assert(unwrap_fenced_block("```cpp\nint x = 1;\n```") == "int x = 1;");
    kak_assert(unwrap_fenced_block("```\nplain\n```") == "plain");
    kak_assert(unwrap_fenced_block("here you go:\n```\nx\n```") == "here you go:\n```\nx\n```");
    kak_assert(unwrap_fenced_block("```\nunbalanced") == "```\nunbalanced");
    kak_assert(unwrap_fenced_block("trailing\n") == "trailing");

    // prompt builder mentions the parts
    String prompt = build_prompt("a.cc", "cpp", "make it const", "int x;");
    kak_assert(contains(prompt, "a.cc"));
    kak_assert(contains(prompt, "cpp"));
    kak_assert(contains(prompt, "make it const"));
    kak_assert(contains(prompt, "--- SELECTION ---\nint x;"));
}};

}
