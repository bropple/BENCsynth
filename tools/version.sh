#!/bin/sh
#
# Prints the version in src/core/bs_version.h, as MAJOR.MINOR.PATCH.
#
# A script rather than a line in the Makefile, because that line cannot be
# written portably. It has to match "#define", and GNU make 3.81 - which is
# what macOS ships - treats a # inside $(shell ...) as the start of a comment,
# truncates the line, and reports "unterminated call to function shell". Make
# 4.x does not, so it worked everywhere except the platform that matters most
# for the plugins.
#
# awk rather than sed for a second reason: BSD sed has no \(a\|b\) alternation.
#
# Everything that needs the number calls this - the Makefile, the version
# check, and the CI jobs that configure cmake without going through make.
set -eu
cd "$(dirname "$0")/.."
awk '/^#define BS_VERSION_(MAJOR|MINOR|PATCH)/ {printf "%s%s", sep, $3; sep="."}' \
    src/core/bs_version.h
