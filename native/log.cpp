#include "log.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/time.h>

#include "decklink_shim.h"

static bool g_verbose = false;

// The log file, and the lock that serialises writes to it.  Lines arrive from
// the feed thread, the decode thread, the control thread and — for anything the
// shim reports — DeckLink's own completion thread, so interleaved fragments
// would otherwise be routine rather than rare.
static std::mutex  g_mutex;
static FILE       *g_file = nullptr;
static std::string g_file_path;
static long        g_file_bytes = 0;

// Big enough to hold a long session's heartbeats (a few hundred bytes a
// minute), small enough that two of them in the plugin's data folder go
// unnoticed.
static constexpr long kMaxLogBytes = 4L * 1024 * 1024;

void log_set_verbose(bool on)
{
    g_verbose = on;
}

const char *log_file_path(void)
{
    return g_file_path.c_str();
}

// Wall-clock time, to the millisecond: the whole point of the file is to line
// dropouts up against when the user saw the picture go, and against IINA's own
// log, which timestamps in the same ISO form.
static void timestamp(char *out, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_buf;
    localtime_r(&tv.tv_sec, &tm_buf);
    char base[32];
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    snprintf(out, len, "%s.%03d", base, (int)(tv.tv_usec / 1000));
}

// Called with g_mutex held.
static void rotate_locked(void)
{
    if (!g_file || g_file_bytes < kMaxLogBytes)
        return;
    fclose(g_file);
    g_file = nullptr;
    std::string previous = g_file_path + ".1";
    remove(previous.c_str());
    rename(g_file_path.c_str(), previous.c_str());
    g_file = fopen(g_file_path.c_str(), "a");
    g_file_bytes = 0;
}

bool log_set_file(const char *path)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    g_file_path.clear();
    g_file_bytes = 0;

    if (!path || !path[0])
        return true;

    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "[error] could not open the log file %s: %s\n",
                path, strerror(errno));
        fflush(stderr);
        return false;
    }
    g_file      = f;
    g_file_path = path;
    // Measured explicitly rather than trusted from ftell(): an append-mode
    // stream's reported position is not portably the end of the file, and
    // getting it wrong means a log that never rotates.  Appending is unaffected
    // by the seek — in "a" mode every write goes to the end regardless.
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    g_file_bytes = size > 0 ? size : 0;
    return true;
}

// `to_stderr` is false for debug lines outside --verbose: they still belong in
// the file, which is the record a dropout gets reconstructed from, but repeating
// them into IINA's log window would bury everything else there.
static void emit(const char *tag, bool to_stderr, const char *fmt, va_list ap)
{
    char text[1024];
    vsnprintf(text, sizeof(text), fmt, ap);

    std::lock_guard<std::mutex> guard(g_mutex);
    if (to_stderr) {
        fprintf(stderr, "[%s] %s\n", tag, text);
        fflush(stderr);
    }
    if (g_file) {
        char when[40];
        timestamp(when, sizeof(when));
        int n = fprintf(g_file, "%s [%s] %s\n", when, tag, text);
        // Flushed on every line on purpose: the interesting case is the one
        // where the helper is killed, wedges, or crashes, and a buffered tail
        // is exactly the part that would explain it.
        fflush(g_file);
        if (n > 0)
            g_file_bytes += n;
        rotate_locked();
    }
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("error", true, fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("warn", true, fmt, ap);
    va_end(ap);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("info", true, fmt, ap);
    va_end(ap);
}

void log_debug(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("debug", g_verbose, fmt, ap);
    va_end(ap);
}

static void shim_log(int level, const char *msg)
{
    switch (level) {
    case 0:  log_error("%s", msg); break;
    case 1:  log_info("%s", msg);  break;
    case 3:  log_warn("%s", msg);  break;
    default: log_debug("%s", msg); break;
    }
}

void log_install_shim_callback(void)
{
    dlk_set_log_callback(shim_log);
}
