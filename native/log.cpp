#include "log.h"

#include <cstdarg>
#include <cstdio>

#include "decklink_shim.h"

static bool g_verbose = false;

void log_set_verbose(bool on)
{
    g_verbose = on;
}

static void emit(const char *tag, const char *fmt, va_list ap)
{
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("error", fmt, ap);
    va_end(ap);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit("info", fmt, ap);
    va_end(ap);
}

void log_debug(const char *fmt, ...)
{
    if (!g_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    emit("debug", fmt, ap);
    va_end(ap);
}

static void shim_log(int level, const char *msg)
{
    switch (level) {
    case 0:  log_error("%s", msg); break;
    case 1:  log_info("%s", msg);  break;
    default: log_debug("%s", msg); break;
    }
}

void log_install_shim_callback(void)
{
    dlk_set_log_callback(shim_log);
}
