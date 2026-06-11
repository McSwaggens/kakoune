#ifndef lsp_hh_INCLUDED
#define lsp_hh_INCLUDED

#include "lsp_client.hh"
#include "coord.hh"
#include "hash_map.hh"
#include "json.hh"
#include "string.hh"
#include "unique_ptr.hh"
#include "utils.hh"
#include "vector.hh"

namespace Kakoune
{

class Context;
class Buffer;

// Native LSP client. Owns one language-server process per (language, project
// root), associates buffers with their server, and translates between LSP and
// Kakoune. Created once, in the server process (see main.cc run_server), and
// registers its options/commands/hooks from the constructor.
class LSPManager : public Singleton<LSPManager>
{
public:
    LSPManager();
    ~LSPManager();

    // Command entry points (invoked from the registered :lsp-* commands).
    void start(Context& context);     // ensure a server for the buffer + didOpen
    void auto_start(Context& context); // start() iff a server is configured; quiet otherwise
    void stop(Context& context);      // shutdown the buffer's server
    void sync(Context& context);      // flush pending didChange for the buffer
    void definition(Context& context);
    void hover(Context& context);
    void complete(Context& context);
    void semantic_tokens(Context& context);
    void references(Context& context);
    void rename(Context& context, StringView new_name);
    void rename_prompt(Context& context); // interactive rename, prefilled with the symbol
    void formatting(Context& context);    // named to not shadow Kakoune::format()
    void code_actions(Context& context);
    void apply_code_action(Context& context, int index);
    void did_close(StringView buffer_name);
    void exit_all();

private:
    struct DocState
    {
        int version = 0;
        size_t synced_timestamp = (size_t)-1;
        bool open = false;
    };

    struct Server
    {
        String key;
        String language_id;
        String root;
        UniquePtr<LSPClient> client;
        bool initialized = false;
        Value capabilities;
        Vector<String> semantic_token_types;     // server's semanticTokens type legend (empty = unsupported)
        Vector<String> semantic_token_modifiers; // server's semanticTokens modifier legend
        HashMap<String, DocState, MemoryDomain::Values> docs; // by buffer filename
        Vector<std::pair<String, String>> queued; // (method, params) until `initialized`
    };

    Server* server_for_buffer(Buffer& buffer); // existing server, or nullptr
    Server* ensure_server(Buffer& buffer, Context& spawn_ctx);
    void send_did_open(Server& server, Buffer& buffer);
    void send_did_change(Server& server, Buffer& buffer);
    void on_notification(Server& server, StringView method, const Value& params);
    void publish_diagnostics(const Value& params);
    void notify(Server& server, StringView method, String params_json);

    // A published diagnostic, kept so codeAction requests can pass the
    // diagnostics overlapping the selection (quick-fixes need this context).
    struct StoredDiagnostic
    {
        String json;            // the diagnostic, as the server sent it
        BufferCoord begin, end; // parsed range (end exclusive, like LSP)
    };

    Vector<UniquePtr<Server>> m_servers;
    Vector<Value> m_code_actions; // last code-action results, indexed by the menu
    HashMap<String, Vector<StoredDiagnostic>> m_diagnostics; // by buffer filename
};

}

#endif // lsp_hh_INCLUDED
