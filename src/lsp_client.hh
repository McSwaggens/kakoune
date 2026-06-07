#ifndef lsp_client_hh_INCLUDED
#define lsp_client_hh_INCLUDED

#include "event_manager.hh"
#include "shell_manager.hh"
#include "json.hh"
#include "hash_map.hh"
#include "function.hh"
#include "optional.hh"
#include "string.hh"

namespace Kakoune
{

class Context;

// A single language-server process speaking JSON-RPC 2.0 over its stdin/stdout
// using the LSP base protocol framing (Content-Length headers). This class is a
// pure transport: it knows nothing about Buffers, Contexts or editor state. All
// I/O happens on the main thread through the EventManager, so callbacks fire
// synchronously from within the event loop and need no locking.
class LSPClient
{
public:
    // result is the "result" member (empty Value for a null/absent result),
    // error is the "error" member (empty Value when the call succeeded).
    // Both are passed by value (moved) so callbacks may retain them; Value is
    // move-only, so they cannot be copied out of a const reference.
    using ResponseCallback = Function<void (Value result, Value error)>;
    using NotificationHandler = Function<void (StringView method, Value params)>;

    // Spawns `cmdline` (through the user's shell) and wires its pipes into the
    // event loop. spawn_ctx is only used during construction (for ShellManager)
    // and is not retained.
    LSPClient(StringView cmdline, const Context& spawn_ctx, NotificationHandler on_notification);
    ~LSPClient();

    LSPClient(const LSPClient&) = delete;
    LSPClient& operator=(const LSPClient&) = delete;

    // Send a request. `params_json` must be an already-serialized JSON value
    // (object/array/null). `cb` runs once when the matching response arrives, or
    // with an error Value if the server dies first. Returns the request id.
    int send_request(StringView method, String params_json, ResponseCallback cb);
    // Send a notification (no response expected).
    void send_notification(StringView method, String params_json);

    bool is_dead() const { return m_dead; }

private:
    void on_readable();
    void on_writable();
    void drain_messages();
    void dispatch(Value message);
    void reply_null(const Value& id);
    void queue_write(String framed);
    void mark_dead();
    void log(StringView what);

    Shell m_shell;

    String m_read_buf;
    Optional<size_t> m_expected_len; // body length once a header block is parsed
    String m_write_buf;              // bytes pending write to the server's stdin
    String m_stderr_buf;             // partial stderr line buffer (for debug log)

    int m_next_id = 1;
    HashMap<int, ResponseCallback> m_pending;
    NotificationHandler m_on_notification;
    bool m_dead = false;

    // Declared last so they are destroyed (unregistered from the EventManager)
    // before the shell's file descriptors close.
    Optional<FDWatcher> m_out_watcher;
    Optional<FDWatcher> m_err_watcher;
    Optional<FDWatcher> m_in_watcher;
};

}

#endif // lsp_client_hh_INCLUDED
