#ifndef fim_hh_INCLUDED
#define fim_hh_INCLUDED

#include "coord.hh"
#include "event_manager.hh"
#include "optional.hh"
#include "string.hh"
#include "unique_ptr.hh"
#include "utils.hh"

namespace Kakoune
{

class Context;
class Buffer;
class HttpRequest;
struct FIMServerProcess;

// Native AI fill-in-middle completion. Spawns the configured completion
// server (llama.cpp's llama-server) as a subprocess, posts the code around
// the cursor to its /infill endpoint over HTTP, and previews the returned
// completion through the ghost-text highlighter. Created once, in the server
// process (see main.cc run_server), and registers its options/commands/hooks
// from the constructor; inert until the fim_cmd option is set.
class FIMManager : public Singleton<FIMManager>
{
public:
    FIMManager();
    ~FIMManager();

    // Command entry points (invoked from the registered :fim-* commands).
    void enable_window(Context& context);  // highlighter + hooks + accept keys
    void auto_enable(Context& context);    // enable_window() iff fim_cmd is set; quiet otherwise
    void disable_window(Context& context);
    void request(Context& context);        // InsertIdle: ask the server for a completion
    void on_insert_char(Context& context); // InsertChar: consume the ghost as it is typed out
    void clear(Context& context);          // movement/delete/mode change: drop ghost + in-flight
    void accept(Context& context);         // insert the ghost text at the cursor
    void accept_line(Context& context);    // insert the ghost's first line, keep the rest
    void menu_hide(Context& context);      // menu closed: drop the preview if it went stale
    void stop(Context& context);           // kill the completion server

private:
    struct Ghost
    {
        String bufname;
        BufferCoord anchor;  // where the ghost text begins (== cursor when set)
        String text;
        size_t timestamp;    // buffer timestamp the ghost is valid for
    };
    struct PendingRequest
    {
        String bufname;
        size_t timestamp;
    };
    struct Stream
    {
        size_t generation;
        String bufname;
        BufferCoord anchor;
        size_t timestamp;
        String event_buffer;
        String pending_text;
    };

    bool ghost_intact_at(const Buffer& buffer, BufferCoord cursor) const;
    void start_server_ifn(const Context& context);
    void send_infill(Buffer& buffer, BufferCoord cursor);
    void request_finished(size_t generation, StringView bufname, BufferCoord anchor, size_t timestamp,
                          bool streamed, bool ok, int status, String body);
    void stream_data(size_t generation, StringView bufname, BufferCoord anchor, size_t timestamp,
                     String data);
    void flush_stream();
    void cancel_request();
    void set_ghost(Buffer& buffer, Ghost ghost);
    void clear_ghost();
    void schedule_retry(String bufname, BufferCoord anchor, size_t timestamp);
    void retry();
    void accept_impl(Context& context, bool line_only);

    Optional<Ghost> m_ghost;
    UniquePtr<HttpRequest> m_request;        // at most one in flight
    Optional<PendingRequest> m_pending_request;
    Optional<Stream> m_stream;
    size_t m_request_generation = 0;         // bumped to ignore stale callbacks
    UniquePtr<FIMServerProcess> m_server;
    bool m_disabled = false;                 // fim-disable persists across windows until fim-enable
    int m_retry_count = 0;
    struct { String bufname; BufferCoord anchor; size_t timestamp; } m_retry;
    Timer m_retry_timer; // server-warmup retries (InsertIdle only fires once per pause)
    Timer m_stream_timer;
};

}

#endif // fim_hh_INCLUDED
