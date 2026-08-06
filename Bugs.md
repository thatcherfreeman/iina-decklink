1. Sometimes the decklink signal drops with no discernable cause, and the
   output doesn't recover until Send to DeckLink is toggled off and on.

   Instrumented rather than fixed so far: the helper now writes @data/helper.log
   (Plugin → Reveal DeckLink Logs…), which carries the card's own state every
   five seconds and an explicit report the moment the card stops accepting
   frames. Next time it happens, that log should say which of these it is:

   - the card stops retiring scheduled frames — `inflight` pinned at its limit
     and `done`/`stream` frozen across consecutive heartbeats, while the feed
     loop backs off forever. This is the one the symptom most resembles, and
     nothing in the current code can recover from it.
   - the driver flushes the queue (`flushed` climbing) — a mode, profile or
     device change underneath the open output.
   - scheduled playback ends underneath us — `ScheduledPlaybackHasStopped`,
     logged as "the card stopped scheduled playback". Scheduling keeps
     succeeding afterwards, so nothing else notices.
   - the scheduling calls start failing outright — `schederr`/`auderr` climbing
     with an HRESULT, which is what a device that has gone away looks like.
   - the helper process exits (crash, or its control socket dropping), which
     the plugin logs as "helper exited unexpectedly".

   Whichever it turns out to be, the recovery is the open question: reopening
   the output is the only thing known to bring it back, and the helper doesn't
   currently attempt it on its own.
