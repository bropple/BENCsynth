#include "bs_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace bs {

static char g_path[512];

const char *bs_log_path()
{
    if (g_path[0]) return g_path;

#if defined(_WIN32)
    const char *base = std::getenv("LOCALAPPDATA");
    std::snprintf(g_path, sizeof g_path, "%s\\BENCsynth.log",
                  base && *base ? base : ".");
#else
    const char *home = std::getenv("HOME");
    if (!home || !*home) home = "/tmp";
#  if defined(__APPLE__)
    /* Where Console.app looks, and where a person is told to look. */
    char dir[400];
    std::snprintf(dir, sizeof dir, "%s/Library/Logs", home);
    mkdir(dir, 0755);
    std::snprintf(g_path, sizeof g_path, "%s/BENCsynth.log", dir);
#  else
    std::snprintf(g_path, sizeof g_path, "%s/.bencsynth.log", home);
#  endif
#endif
    return g_path;
}

void bs_log(const char *fmt, ...)
{
    const char *path = bs_log_path();

    /* Truncate rather than grow without bound. A plugin that quietly fills a
     * disk over months is a worse bug than the one being diagnosed. */
    {
#if defined(_WIN32)
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa) &&
            fa.nFileSizeLow > 256u * 1024u)
            DeleteFileA(path);
#else
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 256 * 1024) unlink(path);
#endif
    }

    FILE *f = std::fopen(path, "a");
    if (!f) return;

    const std::time_t now = std::time(0);
    char when[32] = "";
    std::tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::strftime(when, sizeof when, "%H:%M:%S", &tmv);
    std::fprintf(f, "%s  ", when);

    std::va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);

    std::fputc('\n', f);
    std::fclose(f);
}

} /* namespace bs */
