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
#include "bs_log.h"

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
/* Where the editor binary might be.
 *
 * `hint` is the directory the plugin itself was loaded from. The plugin and
 * the standalone are separate files and nothing guarantees they were installed
 * together, so this tries the explicit answer first and degrades to guessing.
 *
 * macOS needs more guesses than the others, because the editor there is not a
 * loose executable at all - the standalone is BENCsynth.app, and the binary
 * lives four levels inside it. Dropping the .app next to the .clap is the
 * obvious thing to do and it is what a person does; looking only for a file
 * called `bencsynth` misses it completely, which is a plugin whose window
 * never opens and never says why.
 */
static const char *editorCandidates(int i, const char *hint, char *buf, size_t cap)
{
    const char *env = std::getenv("BENCSYNTH_EDITOR");
    if (i == 0) return env && *env ? env : 0;

    /* The hint verbatim. It is normally the directory the plugin was loaded
     * from, but a caller that already knows the executable should not have to
     * pretend otherwise - the test harness passes one, and so might anyone
     * embedding this. Trying it costs one failed exec. */
    if (i == 1) {
        if (!hint || !*hint) return 0;
        std::snprintf(buf, cap, "%s", hint);
        return buf;
    }
    i--;

#if defined(_WIN32)
    static const char *const REL[] = { "/bencsynth.exe" };
    const char *bare = "bencsynth.exe";
#elif defined(__APPLE__)
    static const char *const REL[] = {
        "/BENCsynth.app/Contents/MacOS/bencsynth",  /* beside the plugin */
        "/bencsynth.app/Contents/MacOS/bencsynth",  /* same, lowercased  */
        "/bencsynth"                                /* a loose binary    */
    };
    const char *bare = "bencsynth";
#else
    static const char *const REL[] = { "/bencsynth" };
    const char *bare = "bencsynth";
#endif
    const int nrel = (int)(sizeof REL / sizeof REL[0]);

    if (i - 1 < nrel) {
        if (!hint || !*hint) return 0;
        std::snprintf(buf, cap, "%s%s", hint, REL[i - 1]);
        return buf;
    }

#if defined(__APPLE__)
    /* Installed normally, rather than dropped beside the plugin. */
    if (i - 1 == nrel) {
        std::snprintf(buf, cap, "/Applications/BENCsynth.app/Contents/MacOS/bencsynth");
        return buf;
    }
    if (i - 1 == nrel + 1) return bare;
    return 0;
#else
    if (i - 1 == nrel) return bare;     /* whatever PATH says */
    return 0;
#endif
}

/* How many editorCandidates() will offer before it runs out. */
#if defined(__APPLE__)
#  define BS_EDITOR_TRIES 7
#else
#  define BS_EDITOR_TRIES 4
#endif

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
    /* Explicitly, because zero is macro one. Everything else in this block
     * means "nothing yet" when it is zero and this does not. */
    m->block->learnMacro.store(-1, std::memory_order_relaxed);
    m->block->learnCc.store(0, std::memory_order_relaxed);
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

bool bs_shm_spawn_editor(const char *exePath, const char *shmName,
                         void **procOut, bool offscreen)
{
    char buf[512];
    for (int i = 0; i < BS_EDITOR_TRIES; i++) {
        const char *exe = editorCandidates(i, exePath, buf, sizeof buf);
        if (!exe) continue;

        char cmd[1024];
        std::snprintf(cmd, sizeof cmd, "\"%s\" --editor \"%s\"%s", exe, shmName,
                      offscreen ? " --offscreen" : "");

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        std::memset(&si, 0, sizeof si);
        std::memset(&pi, 0, sizeof pi);
        si.cb = sizeof si;

        bs_log("  trying editor: %s", exe);
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

    /* Unlinked first: a segment with this name can only be a leftover from a
     * crash, since the name carries our own pid. O_EXCL below therefore never
     * fires in practice - it is there so that if the name ever stops being
     * unique, the failure is an error rather than two plugins quietly sharing
     * one block. */
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
    /* Explicitly, because zero is macro one. Everything else in this block
     * means "nothing yet" when it is zero and this does not. */
    m->block->learnMacro.store(-1, std::memory_order_relaxed);
    m->block->learnCc.store(0, std::memory_order_relaxed);
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

bool bs_shm_spawn_editor(const char *exePath, const char *shmName,
                         void **procOut, bool offscreen)
{
    char buf[512];
    for (int i = 0; i < BS_EDITOR_TRIES; i++) {
        const char *exe = editorCandidates(i, exePath, buf, sizeof buf);
        if (!exe) continue;

        bs_log("  trying editor: %s%s", exe, offscreen ? " --offscreen" : "");
        const pid_t pid = fork();
        if (pid < 0) continue;
        if (pid == 0) {
            /* Child. execlp searches PATH for the bare name and takes an
             * absolute path as-is, which is what the candidate list wants. */
            if (offscreen)
                execlp(exe, exe, "--editor", shmName, "--offscreen", (char *)0);
            else
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
                if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
                    bs_log("    not there");
                else
                    bs_log("    started and exited immediately");
                break;
            }
            struct timespec ts = { 0, 5 * 1000 * 1000 };   /* 5 ms */
            nanosleep(&ts, 0);
            if (t == 19) {
                bs_log("    running (pid %ld)", (long)pid);
                *procOut = (void *)(intptr_t)pid;
                return true;
            }
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
