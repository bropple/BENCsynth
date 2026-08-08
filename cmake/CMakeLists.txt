# Our own top level for the VST3 wrapper.
#
# clap-wrapper's convenience path (cmake/top_level_default.cmake) builds every
# format it thinks the platform can manage, and on MSVC that includes AAX -
# which then demands the Avid SDK and stops the configure dead. AAX is
# GPL3-or-commercial and needs Avid/PACE signing to load anywhere, so it is not
# a thing this project wants by accident.
#
# Adding clap-wrapper as a subdirectory instead means that file never runs and
# we ask for exactly one wrapper.
cmake_minimum_required(VERSION 3.21)

# Before project(), and not negotiable: clap-wrapper uses u8"" string literals,
# which are char8_t from C++20 onward and will not convert to const char*. Its
# own top level pins 17; taken as a subdirectory we inherit the compiler's
# default instead, and a new enough GCC defaults to C++23.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The wrapper and the VST3 SDK are built as static libraries and then linked
# into a shared module, so every object in them has to be position independent.
# Without this the link fails on "relocation R_X86_64_PC32 ... can not be used
# when making a shared object", which is a long way from saying "add -fPIC".
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# MSVC: the static CRT, because the VST3 SDK builds itself that way and mixing
# the two is a link error listing every object in the wrapper
# ("mismatch detected for 'RuntimeLibrary'"). Static also means the plugin does
# not need the Visual C++ redistributable on someone else's machine, which is
# the same reason the CLAP and the executable are linked -static under MinGW.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Objective-C++ on Apple, because both wrappers have .mm sources there -
# auv2_shared.mm, wrappedview.mm and os/macos.mm. Without OBJCXX in this list
# CMake configures happily and then fails at generate time with "required
# internal CMake variable not set: CMAKE_OBJCXX_COMPILE_OBJECT", which does not
# obviously mean "declare the language".
#
# It had never come up: there is no macOS VST3 job, so the only build that
# reached a .mm was this one.
if(APPLE)
    project(bencsynth-wrappers LANGUAGES C CXX OBJC OBJCXX)
else()
    project(bencsynth-wrappers LANGUAGES C CXX)
endif()

if(NOT DEFINED CLAP_WRAPPER_DIR)
    message(FATAL_ERROR "set -DCLAP_WRAPPER_DIR to the clap-wrapper checkout")
endif()

# Handed down from src/core/bs_version.h by the Makefile, so a bundle cannot
# claim a version the synth inside it disagrees with. auval reported exactly
# that: a component at 1.1.1 wrapping a CLAP at 0.1.0, in a release tagged
# v0.2.2.
if(NOT DEFINED BS_VERSION OR BS_VERSION STREQUAL "")
    message(FATAL_ERROR "set -DBS_VERSION (the Makefile reads it from bs_version.h)")
endif()

set(CLAP_WRAPPER_OUTPUT_NAME "bencsynth" CACHE STRING "" FORCE)
set(CLAP_WRAPPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(${CLAP_WRAPPER_DIR} clap-wrapper EXCLUDE_FROM_ALL)

# Each wrapper only if its SDK is actually here.
#
# One CMakeLists declares both, so without these guards configuring for either
# format demands both SDKs - which is how the first AU build failed: make
# au-fetch clones clap-wrapper and the AudioUnit SDK, and the configure then
# stopped dead on a missing VST3 tree it had no use for.
#
# The test is clap-wrapper's own: the file guarantee_vst3sdk checks for.
if(EXISTS "${VST3_SDK_ROOT}/public.sdk/source/main/pluginfactory.cpp")
    add_library(bencsynth_as_vst3 MODULE)
    target_add_vst3_wrapper(
        TARGET bencsynth_as_vst3
        OUTPUT_NAME "bencsynth"
        BUNDLE_IDENTIFIER "net.ropple.bencsynth"
        BUNDLE_VERSION "${BS_VERSION}"
    )
else()
    message(STATUS "bencsynth: no VST3 SDK at '${VST3_SDK_ROOT}' - skipping the VST3 wrapper")
endif()

# AU, on macOS only. The only way into Logic and GarageBand, which between
# them are most of the Macs that will ever open this.
#
# Configured "explicitly" rather than from a CLAP target: the wrapper can read
# a plugin's identity out of a clap_entry it builds in the same CMake project,
# and ours is built by the Makefile instead, so the four-character codes are
# spelled out here. They are permanent - a host remembers a plugin by
# type/subtype/manufacturer, and changing any of them makes every saved
# project stop finding it.
if(APPLE AND EXISTS "${AUDIOUNIT_SDK_ROOT}/src/AudioUnitSDK")
    add_library(bencsynth_as_auv2 MODULE)
    target_add_auv2_wrapper(
        TARGET bencsynth_as_auv2
        OUTPUT_NAME "BENCsynth"
        BUNDLE_IDENTIFIER "net.ropple.bencsynth.auv2"
        BUNDLE_VERSION "${BS_VERSION}"
        MANUFACTURER_NAME "BENCO"
        MANUFACTURER_CODE "BNCO"
        SUBTYPE_CODE "BNCS"
        INSTRUMENT_TYPE "aumu"
    )
elseif(APPLE)
    message(STATUS "bencsynth: no AudioUnit SDK at '${AUDIOUNIT_SDK_ROOT}' - skipping the AU wrapper")
endif()
