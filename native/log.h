/*
 * Logging.  Everything goes to stderr; the plugin captures it via utils.exec's
 * stderrHook and forwards it into IINA's log window, so stdout stays clean for
 * JSON.
 */

#ifndef IINA_DECKLINK_LOG_H
#define IINA_DECKLINK_LOG_H

#define LOG_PRINTF_FMT __attribute__((format(printf, 1, 2)))

void log_error(const char *fmt, ...) LOG_PRINTF_FMT;
void log_info(const char *fmt, ...) LOG_PRINTF_FMT;
void log_debug(const char *fmt, ...) LOG_PRINTF_FMT;

// Routes the shim's own log callback into the same stream.
void log_install_shim_callback(void);

// Debug output is suppressed unless this is enabled (--verbose).
void log_set_verbose(bool on);

#endif  // IINA_DECKLINK_LOG_H
