/*
 * BENCsynth - racks a person saved, as opposed to the ones built in.
 *
 * The plugin's Rack parameter is a chooser, and until now it could only offer
 * the 37 presets compiled into the binary. A rack you built in the standalone
 * was unreachable from a host unless you opened the editor and loaded it by
 * hand - which is exactly the round trip a host with no editor cannot make.
 *
 * So: one folder both halves agree on, scanned once, appended to that
 * enumeration.
 *
 * Where that folder is, in order:
 *
 *   1. $BENCSYNTH_RACKS, if set. The escape hatch, and what the tests use.
 *   2. The per-user folder - %APPDATA%\BENCsynth\racks,
 *      ~/Library/Application Support/BENCsynth/racks, or
 *      $XDG_DATA_HOME/bencsynth/racks.
 *   3. Whatever `extra` the caller passes, which is how a portable install
 *      keeps working: the standalone saves beside itself, and the plugin looks
 *      beside the .clap because that is where the installer puts the editor.
 */

#ifndef BS_RACKLIB_H
#define BS_RACKLIB_H

#include <string>

namespace bs {

struct UserRack {
    std::string name;   /* the file's stem, which is what a host displays */
    std::string path;
};

/* The per-user folder. Created on demand when `create` is set, because a
 * standalone about to save into it needs it to exist, and a plugin merely
 * listing it does not. Returns "" if there is no home to put it in. */
const char *userRackDir(bool create = false);

/* Rescan. `extra` may be null; duplicates by name are dropped, earlier
 * directories winning, and the result is sorted so a host's list does not
 * reorder itself between runs. Returns the count. */
int scanUserRacks(const char *extra);

/* Where audio unpacked out of a saved project goes: alongside the racks, so
 * it is somewhere a person can find and not a temporary directory that gets
 * swept. Created on demand. */
const char *userSampleDir();

int             userRackCount();
const UserRack *userRackAt(int i);

}  /* namespace bs */

#endif /* BS_RACKLIB_H */
