/*
 * Platform half of the plugin/editor shared block: mapping it, and starting
 * the process on the other end.
 *
 * Two implementations, and they are genuinely different rather than a thin
 * ifdef - POSIX shared memory is a file that must be unlinked or it outlives
 * everything, and Windows sections are refcounted handles that vanish on their
 * own. Getting that backwards leaks either memory or files.
 */

#include "bs_shm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <signal.h>
#  include <time.h>
#endif

namespace bs {

/* Where the editor binary might be. The plugin and the standalone are
 * separate files and nothing guarantees they are installed together, so this
 * tries the explicit answer first and degrades to guessing. */
static const char *editorCandidates(int i, const char *hint, char *buf, size_t cap)
{
    const char *env = std::getenv("BENCSYNTH_EDITOR");
#if defined(_WIN32)
    const char *exe = "bencsynth.exe";
#else
    const char *exe = "bencsynth";
#endif
    switch (i) {
    case 0: return env && *env ? env : 0;
    case 1:
        if (!hint || !*hint) return 0;
        std::snprintf(buf, cap, "%s", hint);
        return buf;
    case 2: return exe;                 /* whatever PATH says */
    default: return 0;
    }
}

#if defined(_WIN32)

/* ------------------------------------------------------------------ *
 * Windows
 * ------------------------------------------------------------------ */

bool bs_shm_create(ShmMap *m, unsigned long salt)
{
    std::memset(m, 0, sizeof *m);
    m->fd = -1;
    std::snprintf(m->name, sizeof m->name, "Local\\bencsynth-%lu-%lu",
                  (unsigned long)GetCurrentProcessId(), salt);

    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE,
                                  0, (DWORD)sizeof(ShmBlock), m->name);
    if (!h) return false;

    void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmBlock));
    if (!p) { CloseHandle(h); return false; }

    m->handle = h;
    m->block  = (ShmBlock *)p;
    m->owner  = true;
    std::memset(m->block, 0, sizeof(ShmBlock));
    m->block->magic   = BS_SHM_MAGIC;
    m->block->version = BS_SHM_VERSION;
    return true;
}

bool bs_shm_open(ShmMap *m, const char *name)
{
    std::memset(m, 0, sizeof *m);
    m->fd = -1;
    std::snprintf(m->name, sizeof m->name, "%s", name);

    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!h) return false;
    void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShmBlock));
    if (!p) { CloseHandle(h); return false; }

    m->handle = h;
    m->block  = (ShmBlock *)p;
    m->owner  = false;
    if (m->block->magic != BS_SHM_MAGIC || m->block->version != BS_SHM_VERSION) {
        bs_shm_close(m);
        return false;
    }
    return true;
}

void bs_shm_close(ShmMap *m)
{
    if (m->block)  UnmapViewOfFile(m->block);
    if (m->handle) CloseHandle((HANDLE)m->handle);
    m->block = 0;
    m->handle = 0;
}

bool bs_shm_spawn_editor(const char *exePath, const char *shmName, void **procOut)
{
    char buf[512];
    for (int i = 0; i < 3; i++) {
        const char *exe = editorCandidates(i, exePath, buf, sizeof buf);
        if (!exe) continue;

        char cmd[1024];
        std::snprintf(cmd, sizeof cmd, "\"%s\" --editor \"%s\"", exe, shmName);

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        std::memset(&si, 0, sizeof si);
        std::memset(&pi, 0, sizeof pi);
        si.cb = sizeof si;

        if (CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) {
            CloseHandle(pi.hThread);
            *procOut = pi.hProcess;
            return true;
        }
    }
    *procOut = 0;
    return false;
}

void bs_shm_wait_editor(void *proc, int millis)
{
    if (!proc) return;
    WaitForSingleObject((HANDLE)proc, (DWORD)millis);
    CloseHandle((HANDLE)proc);
}

bool bs_shm_editor_running(void *proc)
{
    if (!proc) return false;
    return WaitForSingleObject((HANDLE)proc, 0) == WAIT_TIMEOUT;
}

#else

/* ------------------------------------------------------------------ *
 * POSIX
 * ------------------------------------------------------------------ */

bool bs_shm_create(ShmMap *m, unsigned long salt)
{
    std::memset(m, 0, sizeof *m);
    m->fd = -1;
    std::snprintf(m->name, sizeof m->name, "/bencsynth-%ld-%lu",
                  (long)getpid(), salt);

    /* O_EXCL so a stale segment with the same name is an error rather than a
     * silent share with somebody else's plugin. */
    shm_unlink(m->name);
    int fd = shm_open(m->name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return false;

    if (ftruncate(fd, (off_t)sizeof(ShmBlock)) != 0) {
        close(fd);
        shm_unlink(m->name);
        return false;
    }
    void *p = mmap(0, sizeof(ShmBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        shm_unlink(m->name);
        return false;
    }

    m->fd    = fd;
    m->block = (ShmBlock *)p;
    m->owner = true;
    std::memset(m->block, 0, sizeof(ShmBlock));
    m->block->magic   = BS_SHM_MAGIC;
    m->block->version = BS_SHM_VERSION;
    return true;
}

bool bs_shm_open(ShmMap *m, const char *name)
{
    std::memset(m, 0, sizeof *m);
    m->fd = -1;
    std::snprintf(m->name, sizeof m->name, "%s", name);

    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) return false;
    void *p = mmap(0, sizeof(ShmBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return false; }

    m->fd    = fd;
    m->block = (ShmBlock *)p;
    m->owner = false;
    if (m->block->magic != BS_SHM_MAGIC || m->block->version != BS_SHM_VERSION) {
        bs_shm_close(m);
        return false;
    }
    return true;
}

void bs_shm_close(ShmMap *m)
{
    if (m->block) munmap(m->block, sizeof(ShmBlock));
    if (m->fd >= 0) close(m->fd);
    /* Only the creator unlinks. A POSIX segment outlives every process that
     * mapped it, so skipping this leaves litter in /dev/shm until reboot. */
    if (m->owner && m->name[0]) shm_unlink(m->name);
    m->block = 0;
    m->fd = -1;
}

bool bs_shm_spawn_editor(const char *exePath, const char *shmName, void **procOut)
{
    char buf[512];
    for (int i = 0; i < 3; i++) {
        const char *exe = editorCandidates(i, exePath, buf, sizeof buf);
        if (!exe) continue;

        const pid_t pid = fork();
        if (pid < 0) continue;
        if (pid == 0) {
            /* Child. execlp searches PATH for the bare name and takes an
             * absolute path as-is, which is what the candidate list wants. */
            execlp(exe, exe, "--editor", shmName, (char *)0);
            _exit(127);          /* exec failed - do not run atexit handlers */
        }

        /* Give a failed exec a moment to become a dead child, so a bad
         * candidate falls through to the next rather than being reported as a
         * running editor that never draws anything. */
        for (int t = 0; t < 20; t++) {
            int status = 0;
            const pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 127) break;
                break;
            }
            struct timespec ts = { 0, 5 * 1000 * 1000 };   /* 5 ms */
            nanosleep(&ts, 0);
            if (t == 19) { *procOut = (void *)(intptr_t)pid; return true; }
        }
    }
    *procOut = 0;
    return false;
}

void bs_shm_wait_editor(void *proc, int millis)
{
    const pid_t pid = (pid_t)(intptr_t)proc;
    if (pid <= 0) return;
    for (int t = 0; t < millis / 5; t++) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) return;
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, 0);
    }
    /* It had its chance. */
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
}

bool bs_shm_editor_running(void *proc)
{
    const pid_t pid = (pid_t)(intptr_t)proc;
    if (pid <= 0) return false;
    int status = 0;
    return waitpid(pid, &status, WNOHANG) == 0;
}

#endif

} /* namespace bs */
