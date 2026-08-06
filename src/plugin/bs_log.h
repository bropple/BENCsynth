/*
 * A log file, because a plugin has nowhere else to speak.
 *
 * When something goes wrong inside a host there is no console: stderr is
 * swallowed, there is no window yet to put a message in, and the host reports
 * "the plugin did not open" whatever the actual reason was. Two very different
 * failures - the host never asking for a GUI, and the editor process failing
 * to start - look identical from outside, and both have now been diagnosed by
 * guessing. This is cheaper than guessing.
 *
 * A few lines per session, appended, capped. Always on: a log nobody has to
 * enable is a log that exists when it is needed, and the cost is a file.
 */
#ifndef BS_LOG_H
#define BS_LOG_H

namespace bs {

/* ~/Library/Logs/BENCsynth.log on macOS, $HOME/.bencsynth.log elsewhere,
 * %LOCALAPPDATA%\BENCsynth.log on Windows. Returns the path it uses. */
const char *bs_log_path();

void bs_log(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
;

} /* namespace bs */

#endif
