/*
 * Logging.  Everything goes to stderr; the plugin captures it via utils.exec's
 * stderrHook and forwards it into IINA's log window, so stdout stays clean for
 * JSON.
 *
 * stderr alone is not enough to diagnose a fault that shows up once a week: it
 * lands in an in-memory log window that is gone by the time anyone thinks to
 * look.  So everything is *also* written to a file when one is configured
 * (--log, or the plugin passing @data/helper.log), including the debug lines
 * --verbose would be needed to see on stderr.  That file is what a dropout
 * report is reconstructed from.
 */

#ifndef IINA_DECKLINK_LOG_H
#define IINA_DECKLINK_LOG_H

#define LOG_PRINTF_FMT __attribute__((format(printf, 1, 2)))

void log_error(const char *fmt, ...) LOG_PRINTF_FMT;
void log_warn(const char *fmt, ...) LOG_PRINTF_FMT;
void log_info(const char *fmt, ...) LOG_PRINTF_FMT;
void log_debug(const char *fmt, ...) LOG_PRINTF_FMT;

// Routes the shim's own log callback into the same stream.
void log_install_shim_callback(void);

// Debug output is suppressed unless this is enabled (--verbose).  The log file,
// when one is open, records debug lines either way.
void log_set_verbose(bool on);

// Mirrors every line into `path`, timestamped, appending to whatever is already
// there.  The file is rotated to `path.1` once it passes a few megabytes, so a
// long-running session can't fill the data folder.  Returns false (and logs why)
// if the file can't be opened; logging to stderr is unaffected either way.
// Passing nullptr or "" closes any file currently open.
bool log_set_file(const char *path);

// Where the log file went, or "" if none is open — reported at startup so a bug
// report knows which file to attach.
const char *log_file_path(void);

#endif  // IINA_DECKLINK_LOG_H
