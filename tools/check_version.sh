#!/bin/sh
#
# One version, everywhere.
#
# src/core/bs_version.h has always said it is the single place the window, the
# info panel and anything that packages a build read from. It was not: the CLAP
# descriptor, its Info.plist and both cmake bundle versions each carried their
# own literal, and nothing compared them. auval on v0.2.2 printed the result -
# a component reporting 1.1.1, wrapping a CLAP reporting 0.1.0, in a release
# tagged v0.2.2. Three numbers, no two alike.
#
# So: no literal versions outside the header, and on a release, the tag has to
# match it too.
#
#   tools/check_version.sh            consistency
#   tools/check_version.sh v0.2.3     consistency, and that the tag agrees
#
set -eu
cd "$(dirname "$0")/.."

V=$(tools/version.sh)
case "$V" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "cannot read a version out of src/core/bs_version.h (got '$V')"; exit 1 ;;
esac
echo "  bs_version.h  $V"

fail=0

# Nothing else may spell a version out. The check is for a three-number literal
# in the files that used to carry one; @BS_VERSION@ and BS_VERSION_STRING are
# what they should say instead.
for f in src/clap/bencsynth_clap.cpp src/clap/Info.plist cmake/CMakeLists.txt; do
    if grep -nE '"[0-9]+\.[0-9]+\.[0-9]+"|>[0-9]+\.[0-9]+\.[0-9]+<' "$f"; then
        echo "  $f has a version literal - it should take BS_VERSION instead"
        fail=1
    else
        echo "  ok            $f"
    fi
done

# The tag, when there is one.
if [ $# -gt 0 ]; then
    want=${1#v}
    if [ "$want" != "$V" ]; then
        echo
        echo "  tag $1 does not match bs_version.h ($V)."
        echo "  bump src/core/bs_version.h, or tag v$V."
        fail=1
    else
        echo "  ok            tag $1"
    fi
fi

[ "$fail" = 0 ] || exit 1
echo "  one version, everywhere: $V"
