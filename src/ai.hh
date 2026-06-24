#ifndef ai_hh_INCLUDED
#define ai_hh_INCLUDED

#include "coord.hh"
#include "optional.hh"
#include "selection.hh"
#include "string.hh"
#include "unique_ptr.hh"
#include "utils.hh"
#include "vector.hh"

namespace Kakoune
{

class Context;
class AIRequest;

// AI-assisted editing on selections. The `ai` command sends each selection's
// text plus a shared instruction to an agentic CLI (codex by default, or
// claude) running read-only, and replaces each selection with the returned
// text as one undo group. The CLIs run as one-shot subprocesses on the event
// loop; we apply the result ourselves so the edit is a normal undoable change.
// Created once, in the server process (see main.cc run_server); inert until
// invoked.
class AIManager : public Singleton<AIManager>
{
public:
    AIManager();
    ~AIManager();

    void rewrite(Context& context, StringView instruction); // :ai with an argument
    void rewrite_prompt(Context& context);                  // :ai with no argument
    void cancel(Context& context);                          // :ai-cancel
    void cancel_all();                                       // KakEnd

private:
    // One in-flight batch: N selections sharing an instruction, each rewritten
    // by its own subprocess, all applied together when the last one returns.
    struct Edit
    {
        size_t generation;
        String client_name;
        String buffer_name;
        size_t timestamp;             // buffer timestamp at launch; mismatch ⇒ abort
        bool json_result;             // claude (parse stdout JSON) vs codex (read outfile)
        Vector<Selection> selections; // sorted snapshot
        Vector<String> originals;     // selection texts (fallback for failures)
        Vector<String> commands;      // shell command per selection
        Vector<String> prompts;       // prompt fed on stdin per selection
        Vector<String> outfiles;      // codex -o path per selection ("" for claude)
        Vector<Optional<String>> results;
        size_t next_index = 0;        // next selection to launch
        size_t outstanding = 0;       // requests not yet returned
        size_t running = 0;           // requests currently in flight
        Vector<UniquePtr<AIRequest>> active;
    };

    void launch(Context& context, StringView instruction);
    void request_done(size_t generation, size_t index, bool ok, String text);
    void start_next(Edit& edit);
    void finish_edit(Edit& edit);
    void set_status(StringView client_name, String message);

    UniquePtr<Edit> m_edit;   // at most one batch in flight
    size_t m_generation = 0;  // bumped on cancel; stale callbacks are dropped
};

}

#endif // ai_hh_INCLUDED
