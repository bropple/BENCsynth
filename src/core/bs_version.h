/*
 * BENCsynth - version
 *
 * One place, so the window, the info panel, the macOS bundle's Info.plist and
 * anything that packages a build all agree. A release tag is `v` followed by
 * these three numbers.
 */

#ifndef BS_VERSION_H
#define BS_VERSION_H

#define BS_VERSION_MAJOR 0
#define BS_VERSION_MINOR 2
#define BS_VERSION_PATCH 3

#define BS_STR2(x) #x
#define BS_STR(x)  BS_STR2(x)

#define BS_VERSION_STRING \
    BS_STR(BS_VERSION_MAJOR) "." BS_STR(BS_VERSION_MINOR) "." BS_STR(BS_VERSION_PATCH)

#endif /* BS_VERSION_H */
