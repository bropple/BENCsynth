# BENCsynth
#
#   make            the standalone synthesizer
#   make test       the core test, which needs no raylib and no sound card
#   make render     a phrase through the default rack, written to a .wav
#   make icons      regenerate assets/icon from the star geometry in the code
#   make lv2        the LV2 plugin, for LMMS and every other LV2 host
#   make lv2-test   load the bundle in a tiny host and play it
#   make lv2-install    copy the bundle where a host will find it
#   make core       just the DSP library
#   make info       what the build decided about raylib, when it decided wrong
#
# The core is built without raylib on purpose. It is the part a plugin will
# link, and a dependency on a windowing library is exactly the thing that
# makes that impossible later.

CXX      ?= c++
AR       ?= ar
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -MMD -MP
CPPFLAGS += -Isrc/core -Itests

UNAME_S := $(shell uname -s)

# gcc appends .exe on Windows no matter what -o says. A target named without it
# is a target make never finds, so every invocation relinks and nothing is ever
# up to date.
ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  EXE :=
endif

CORE_SRC := src/core/bs_patch.cpp \
            src/core/bs_modules.cpp \
            src/core/bs_engine.cpp \
            src/core/bs_patchfile.cpp
CORE_OBJ := $(CORE_SRC:.cpp=.o)
CORE_LIB := libbencsynth.a

# The editor runs in another process and talks to the plugin over shared
# memory. Both halves need this, and the standalone IS the editor (--editor),
# so it is linked into the program as well as into the plugins.
# Shared by the plugin and the editor.
PLUGIN_SRC := src/plugin/bs_shm.cpp src/plugin/bs_sync.cpp src/plugin/bs_log.cpp

# The editor's alone. bs_embed reaches into GLFW for the window id raylib will
# not hand over, so it can only be linked where raylib is - and the plugin has
# no raylib at all. Building it into the plugin left glfwGetX11Window undefined
# in the .clap, which a shared object accepts at link time and a host discovers
# at load time, or worse resolves against its own GLFW.
EDITOR_SRC := src/plugin/bs_embed.cpp
# Listed as prerequisites everywhere PLUGIN_SRC is used. These describe a
# struct that lives in shared memory and is read by two separate binaries: if
# one is rebuilt against a new layout and the other is not, they map the same
# bytes and disagree about what is in them, which looks exactly like an editor
# that attaches and then does nothing.
ifeq ($(UNAME_S),Linux)
  IPC_X11 := -lX11
endif

PLUGIN_HDR := src/plugin/bs_shm.h src/plugin/bs_sync.h src/plugin/bs_embed.h \
              src/plugin/bs_log.h src/plugin/bs_cocoa.h

GUI_SRC  := src/gui/main.cpp \
            src/gui/bs_gui.cpp \
            src/gui/bs_rope.cpp \
            src/gui/bs_rack.cpp \
            src/gui/bs_keyboard.cpp \
            src/gui/bs_filedlg.cpp \
            src/gui/bs_input.cpp
# The standalone is also the plugin's editor (--editor), so it carries the
# same shared-memory half the plugin does.
GUI_SRC  += $(PLUGIN_SRC) $(EDITOR_SRC)
GUI_OBJ  := $(GUI_SRC:.cpp=.o)
GUI      := bencsynth$(EXE)

TEST_SRC := tests/test_core.cpp tests/test_patchfile.cpp
TEST_OBJ := $(TEST_SRC:.cpp=.o)
TEST     := bencsynth-test$(EXE)

# ------------------------------------------------------------------
# LV2
#
# LMMS cannot load VST3 and will not be given a VST2 - see docs/PLUGIN.md.
# LV2 is a plain C API with headers and no SDK licence, which is why this is
# forty lines of makefile rather than a vendored tree.
#
# A bundle is a directory: two Turtle files describing the plugin and one
# shared object implementing it.
# ------------------------------------------------------------------

LV2_SRC    := src/lv2/bencsynth_lv2.cpp
LV2_BUNDLE := build/bencsynth.lv2

# Both architectures in one binary. macos-latest runners are Apple Silicon, so
# a plain build is arm64-only and will not load in an Intel host at all -
# which, like everything else on this platform, is reported as "damaged".
MAC_ARCHS := -arch x86_64 -arch arm64

# The plugins are universal unconditionally, above. The executable is not, and
# for a long time that was only a standalone-download question: an Intel Mac
# got nothing, and could tell.
#
# It stopped being only that when the CLAP started opening its editor by
# running this binary. The plugin bundle carries BENCsynth.app, so an arm64
# executable inside a universal plugin means the plugin loads on an Intel Mac
# and its window never appears - the one failure that looks like the plugin
# being broken rather than the download being wrong.
#
# Off by default all the same. Two -arch flags and -MMD cannot be combined -
# clang refuses outright, which is why every plugin rule below filters them
# out - so turning this on costs dependency tracking, and somebody rebuilding
# one file locally should not pay for a release's problem. Releases and CI set
# it; `make` on a developer's Mac stays native and incremental.
ifeq ($(UNAME_S),Darwin)
ifeq ($(MACOS_UNIVERSAL),1)
  CXXFLAGS := $(filter-out -MMD -MP,$(CXXFLAGS)) $(MAC_ARCHS)
endif
endif

ifeq ($(UNAME_S),Darwin)
  LV2_LIB_EXT := .dylib
  LV2_SHARED  := -dynamiclib $(MAC_ARCHS)
  LV2_LINK    :=
else ifeq ($(OS),Windows_NT)
  LV2_LIB_EXT := .dll
  LV2_SHARED  := -shared
  # Static, for the same reason the executable is: a DLL that imports
  # libstdc++-6 and libwinpthread-1 needs them on the PATH of whatever loads
  # it, and LMMS's PATH is not MSYS2's. The host's LoadLibrary just fails and
  # the plugin is missing with no message - identical symptom to a bundle in
  # the wrong directory, which makes it thoroughly unfun to diagnose.
  LV2_LINK    := -static
else
  LV2_LIB_EXT := .so
  LV2_SHARED  := -shared
  LV2_LINK    :=
endif

# LV2 is header-only to build against, so LV2_INCLUDE lets a build point
# straight at an unpacked release when there is no lv2.pc to find - which is
# the normal situation on Windows and macOS, where there is no distro package
# putting one there.
ifneq ($(LV2_INCLUDE),)
  LV2_CFLAGS := -I$(LV2_INCLUDE)
  LV2_FOUND  := yes
else
  LV2_CFLAGS := $(shell pkg-config --cflags lv2 2>/dev/null)
  LV2_FOUND  := $(shell pkg-config --exists lv2 2>/dev/null && echo yes)
endif

# Where a bundle goes to be found. These are the user-level directories from
# the LV2 filesystem hierarchy standard, which every host searches by default -
# no host-side configuration, no LV2_PATH, nothing to set.
ifeq ($(UNAME_S),Darwin)
  LV2_INSTALL_DIR ?= $(HOME)/Library/Audio/Plug-Ins/LV2
else ifeq ($(OS),Windows_NT)
  LV2_INSTALL_DIR ?= $(APPDATA)/LV2
else
  LV2_INSTALL_DIR ?= $(HOME)/.lv2
endif

# ------------------------------------------------------------------
# CLAP
#
# MIT, no SDK licence, and clap-wrapper turns one of these into VST3 and AU -
# which is how this reaches hosts that will never load an LV2. The SDK is
# headers only, so there is nothing to build and nothing to link.
#
# A CLAP is a single shared library named .clap on Linux and Windows; macOS
# wants a bundle directory, which is why that branch does more.
# ------------------------------------------------------------------

# One version, read from the header rather than repeated here. src/core/
# bs_version.h says it is the single place; it was not - the CLAP descriptor,
# its Info.plist and both cmake bundle versions each carried their own copy,
# and auval reported a plugin claiming 1.1.1 wrapping a CLAP claiming 0.1.0
# in a release tagged v0.2.2.
# One line and awk, not sed over a continuation: macOS ships GNU make 3.81,
# which reads the backslash-newline inside $(shell ...) and then reports
# "unterminated call to function shell". BSD sed would not have taken the
# \(a\|b\) alternation either.
BS_VERSION := $(shell awk '/^#define BS_VERSION_(MAJOR|MINOR|PATCH)/ {printf "%s%s", sep, $$3; sep="."}' src/core/bs_version.h)

CLAP_SRC     := src/clap/bencsynth_clap.cpp $(PLUGIN_SRC)
CLAP_VERSION := 1.2.10
CLAP_INCLUDE ?= vendor/clap/include
CLAP_FOUND   := $(if $(wildcard $(CLAP_INCLUDE)/clap/clap.h),yes,)

ifeq ($(UNAME_S),Darwin)
  CLAP_BUNDLE  := build/bencsynth.clap
  CLAP_BINARY  := $(CLAP_BUNDLE)/Contents/MacOS/bencsynth
  CLAP_SHARED  := -dynamiclib $(MAC_ARCHS)
  # Embedding on this platform means handing pixels between processes rather
  # than reparenting a window, so there is Objective-C++ and there are
  # frameworks. See src/plugin/bs_cocoa.h for why.
  CLAP_EXTRA_SRC := src/plugin/bs_cocoa.mm
  CLAP_FRAMEWORKS := -framework Cocoa -framework QuartzCore -framework IOSurface
  CLAP_LINK    :=
  CLAP_INSTALL_DIR ?= $(HOME)/Library/Audio/Plug-Ins/CLAP
else ifeq ($(OS),Windows_NT)
  CLAP_BUNDLE  := build/bencsynth.clap
  CLAP_BINARY  := $(CLAP_BUNDLE)
  CLAP_SHARED  := -shared
  # Static for the same reason the LV2 and the executable are: a host will not
  # have MSYS2's runtime DLLs on its PATH, and LoadLibrary failing looks
  # exactly like the plugin not being installed.
  CLAP_LINK    := -static
  CLAP_INSTALL_DIR ?= $(LOCALAPPDATA)/Programs/Common/CLAP
else
  CLAP_BUNDLE  := build/bencsynth.clap
  CLAP_BINARY  := $(CLAP_BUNDLE)
  CLAP_SHARED  := -shared
  CLAP_LINK    :=
  # No -lX11: the plugin does not touch X11 at all. The editor does, and it
  # links it through the GUI's own flags.
  CLAP_RT      := -lrt
  CLAP_INSTALL_DIR ?= $(HOME)/.clap
endif

# ------------------------------------------------------------------
# VST3, by wrapping the CLAP
#
# Not a second port. clap-wrapper builds a VST3 that loads a CLAP of the same
# name from the standard CLAP directories at runtime - so this is the same
# plugin, the same editor, reaching Ableton, Cubase, FL Studio and Studio One,
# none of which will ever load an LV2 or a bare CLAP.
#
# Both SDKs are MIT: CLAP always was, and Steinberg relicensed VST3 in October
# 2025. Pinned rather than tracked, because clap-wrapper's main branch follows
# the CLAP SDK closely enough that an unpinned pair stops compiling.
# ------------------------------------------------------------------

CW_VERSION   := v0.15.1
VST3_SDK_TAG := master
CW_DIR       := vendor/clap-wrapper
VST3_SDK_DIR := vendor/vst3sdk
VST3_BUILD   := build/vst3
VST3_BUNDLE  := build/bencsynth.vst3

ifeq ($(UNAME_S),Darwin)
  VST3_INSTALL_DIR ?= $(HOME)/Library/Audio/Plug-Ins/VST3
else ifeq ($(OS),Windows_NT)
  VST3_INSTALL_DIR ?= $(LOCALAPPDATA)/Programs/Common/VST3
else
  VST3_INSTALL_DIR ?= $(HOME)/.vst3
endif

# AU, from the same wrapper. macOS only - there is no such thing anywhere
# else, and the wrapper's own cmake returns early off Apple.
AU_SDK_TAG     := AudioUnitSDK-1.1.0
AU_SDK_DIR     := vendor/AudioUnitSDK
AU_BUILD       := build/au
AU_BUNDLE      := build/BENCsynth.component
AU_INSTALL_DIR ?= $(HOME)/Library/Audio/Plug-Ins/Components

RENDER_SRC := tools/render_wav.cpp
RENDER_OBJ := $(RENDER_SRC:.cpp=.o)
RENDER     := bencsynth-render$(EXE)

# ------------------------------------------------------------------
# raylib
#
# Three places, most specific first:
#
#   vendor/raylib     a prefix dropped into the tree, which is what the
#                     .gitignore expects and what a fresh clone should set up
#   RAYLIB=/prefix    on the command line, for a copy living elsewhere
#   pkg-config        a system package
#
# A static libraylib.a is preferred where one exists, so the binary runs on a
# machine that has never heard of raylib. pkg-config --static is what pulls in
# GLFW, OpenGL and the platform libraries raylib itself needs; a bare -lraylib
# against a static archive fails to link without them.
# ------------------------------------------------------------------

VENDOR_RL := $(wildcard vendor/raylib/lib/libraylib.a)

ifneq ($(VENDOR_RL),)
  RL_CFLAGS := -Ivendor/raylib/include
  RL_LIBS   := $(VENDOR_RL)
  RL_FROM   := vendor/raylib
else ifdef RAYLIB
  RL_CFLAGS := -I$(RAYLIB)/include
  RL_STATIC := $(wildcard $(RAYLIB)/lib/libraylib.a)
  RL_LIBS   := $(if $(RL_STATIC),$(RL_STATIC),-L$(RAYLIB)/lib -lraylib)
  RL_FROM   := RAYLIB=$(RAYLIB)
else
  RL_CFLAGS := $(shell pkg-config --cflags raylib 2>/dev/null)
  RL_LIBS   := $(shell pkg-config --libs --static raylib 2>/dev/null)
  RL_FROM   := pkg-config
endif

# What raylib needs underneath it. pkg-config already answered this; the
# vendored and RAYLIB= paths have to be told.
ifeq ($(RL_FROM),pkg-config)
  RL_SYS :=
else ifeq ($(UNAME_S),Darwin)
  RL_SYS := -framework CoreVideo -framework IOKit -framework Cocoa \
            -framework GLUT -framework OpenGL
else ifeq ($(OS),Windows_NT)
  RL_SYS := -lopengl32 -lgdi32 -lwinmm
else
  RL_SYS := -lGL -lm -lpthread -ldl -lrt -lX11
endif

LDLIBS_GUI := $(RL_LIBS) $(RL_SYS)

# Windows: compile the icon into the executable, link as a GUI subsystem binary
# so double-clicking it does not also open a console behind the window, and
# link the toolchain's own runtime statically.
#
# That last one is not an optimisation. Without -static the binary imports
# libgcc_s_seh-1.dll, libwinpthread-1.dll and libstdc++-6.dll, which live
# inside an MSYS2 installation and nowhere else. The build succeeds, the
# program runs perfectly on the machine that built it, and it fails to start on
# every machine that has just downloaded it - with a missing-DLL dialog naming
# a file the person has never heard of. C++ makes this worse than it is for C:
# the standard library and the thread support both arrive as separate DLLs.
#
# CI asserts the absence of all three against the built .exe, because the only
# place the answer lives is the import table.
ifeq ($(OS),Windows_NT)
  GUI_RES  := src/gui/bencsynth.res.o
  GUI_LINK := -mwindows -static -lcomdlg32
else
  GUI_RES  :=
  GUI_LINK :=
endif


# ------------------------------------------------------------------

.PHONY: all core test clean info run render icons lv2 lv2-install lv2-validate lv2-test \
        clap clap-test clap-install clap-fetch ipc-test \
        vst3 vst3-fetch vst3-install mac-sign macos-plugin-dmg \
        au au-fetch au-install


all: $(GUI)

core: $(CORE_LIB)
# The icon files are generated from bs_star_image(), which is the same code
# that draws the mark in the window - so they cannot drift from it. Committed
# rather than built, because a release job should not need ImageMagick.
icons: $(GUI)
	./tools/make-icons.sh


$(CORE_LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

$(GUI): $(GUI_OBJ) $(GUI_RES) $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $(GUI_LINK) -o $@ $(GUI_OBJ) $(GUI_RES) $(CORE_LIB) $(LDLIBS_GUI) $(CLAP_RT)

$(GUI_OBJ): CPPFLAGS += $(RL_CFLAGS) -Isrc/gui -Isrc/plugin

test: $(TEST)
	./$(TEST)

$(TEST): $(TEST_OBJ) $(CORE_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJ) $(CORE_LIB) -lm

# A phrase through the default rack, written to a file. Needs no raylib and no
# sound card, which makes it the way to hear the synthesizer on a machine that
# has neither.
render: $(RENDER)
	./$(RENDER) bencsynth-demo.wav

$(RENDER): $(RENDER_OBJ) $(CORE_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(RENDER_OBJ) $(CORE_LIB) -lm

run: $(GUI)
	./$(GUI)

# ------------------------------------------------------------------
# The plugin. Position-independent because it is loaded into somebody else's
# process, and the core is compiled again here rather than reusing
# libbencsynth.a, which is not built -fPIC.
# ------------------------------------------------------------------

lv2:
ifeq ($(LV2_FOUND),yes)
	@$(MAKE) --no-print-directory $(LV2_BUNDLE)
else
	@echo "  no LV2 headers found - install the 'lv2' development package,"
	@echo "  or point LV2_INCLUDE at the include/ directory of an unpacked"
	@echo "  LV2 release: LV2_INCLUDE=/path/to/lv2/include make lv2"
	@echo "  Everything else still builds; this target is the plugin only."
	@false
endif

LV2GEN := bencsynth-gen-racks$(EXE)

$(LV2GEN): tools/gen_lv2_racks.cpp $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $< $(CORE_LIB)

$(LV2_BUNDLE): $(LV2_SRC) $(CORE_SRC) src/lv2/bencsynth.ttl.in src/lv2/manifest.ttl.in $(LV2GEN)
	@mkdir -p $(LV2_BUNDLE)
	# -MMD is filtered out: it is for incremental builds, and here it would
	# just leave a .d file sitting inside the bundle a host is meant to read.
	$(CXX) $(filter-out -MMD -MP,$(CXXFLAGS)) $(CPPFLAGS) $(LV2_CFLAGS) \
	    -fPIC -fvisibility=hidden \
	    $(LV2_SHARED) $(LV2_LINK) -o $(LV2_BUNDLE)/bencsynth$(LV2_LIB_EXT) \
	    $(LV2_SRC) $(CORE_SRC) -lm
ifeq ($(UNAME_S),Darwin)
	@$(MAKE) --no-print-directory mac-sign SIGN=$(LV2_BUNDLE)/bencsynth$(LV2_LIB_EXT)
endif
	sed 's|@LIB_EXT@|$(LV2_LIB_EXT)|' src/lv2/manifest.ttl.in > $(LV2_BUNDLE)/manifest.ttl
	./$(LV2GEN) > /tmp/bs-rack-port.ttl
	awk '/@RACK_PORT@/ { while ((getline line < "/tmp/bs-rack-port.ttl") > 0) print line; next } { print }' \
	    src/lv2/bencsynth.ttl.in > $(LV2_BUNDLE)/bencsynth.ttl
	@echo "built $(LV2_BUNDLE)"

# The Turtle has to describe the ports the C actually connects, and nothing in
# the compiler checks that. lv2_validate reads the bundle the way a host will.
lv2-validate: lv2
	@if command -v lv2_validate >/dev/null 2>&1 && \
	    command -v sord_validate >/dev/null 2>&1; then \
	    lv2_validate $(LV2_BUNDLE)/*.ttl && \
	    echo "the bundle describes itself correctly"; \
	 else \
	    echo "  lv2_validate/sord_validate not installed - skipping."; \
	    echo "  lv2_validate is a script that calls sord_validate, and on"; \
	    echo "  Debian they are in different packages: lv2-dev, sord-utils."; \
	 fi

# Loads the bundle the way a host does and plays it. Compiling proves nothing
# about whether the ports are connected to the right buffers or whether state
# survives a round trip; this does.
LV2HOST := bencsynth-lv2-host$(EXE)

lv2-test: lv2 $(LV2HOST)
	./$(LV2HOST) $(LV2_BUNDLE)/bencsynth$(LV2_LIB_EXT)

$(LV2HOST): tools/lv2_host.cpp
	$(CXX) $(CXXFLAGS) $(LV2_CFLAGS) -o $@ $< -ldl

# The whole bundle directory, not the shared object inside it. A bundle is the
# unit LV2 deals in - the .ttl files beside the binary are what tell a host the
# plugin exists at all - so copying only the .so leaves a host with nothing to
# find and no reason given.
lv2-install: lv2
	mkdir -p "$(LV2_INSTALL_DIR)"
	rm -rf "$(LV2_INSTALL_DIR)/bencsynth.lv2"
	cp -r $(LV2_BUNDLE) "$(LV2_INSTALL_DIR)/"
	@echo
	@echo "  installed to $(LV2_INSTALL_DIR)/bencsynth.lv2"
	@echo "  restart the host - it searches there by default."

# ------------------------------------------------------------------
# CLAP targets
# ------------------------------------------------------------------

clap:
ifeq ($(CLAP_FOUND),yes)
	@$(MAKE) --no-print-directory $(CLAP_BINARY)
else
	@echo "  no CLAP headers at $(CLAP_INCLUDE)."
	@echo "  run 'make clap-fetch', or point CLAP_INCLUDE at the include/"
	@echo "  directory of a CLAP checkout."
	@false
endif

# Headers only, and vendor/ is gitignored - same arrangement as raylib.
clap-fetch:
	mkdir -p vendor
	curl -sL -o /tmp/clap-$(CLAP_VERSION).tar.gz \
	    https://github.com/free-audio/clap/archive/refs/tags/$(CLAP_VERSION).tar.gz
	tar xzf /tmp/clap-$(CLAP_VERSION).tar.gz -C vendor
	rm -rf vendor/clap
	mv vendor/clap-$(CLAP_VERSION) vendor/clap
	@echo "CLAP $(CLAP_VERSION) headers in vendor/clap"

$(CLAP_BINARY): $(CLAP_SRC) $(PLUGIN_HDR) $(CORE_SRC) $(wildcard src/clap/Info.plist)
	@mkdir -p $(dir $(CLAP_BINARY))
	$(CXX) $(filter-out -MMD -MP,$(CXXFLAGS)) $(CPPFLAGS) -I$(CLAP_INCLUDE) \
	    -Isrc/plugin -fPIC -fvisibility=hidden \
	    $(CLAP_SHARED) $(CLAP_LINK) -o $(CLAP_BINARY) \
	    $(CLAP_SRC) $(CLAP_EXTRA_SRC) $(CORE_SRC) -lm $(CLAP_RT) $(CLAP_FRAMEWORKS)
ifeq ($(UNAME_S),Darwin)
	sed 's/@BS_VERSION@/$(BS_VERSION)/g' src/clap/Info.plist > $(CLAP_BUNDLE)/Contents/Info.plist
	@$(MAKE) --no-print-directory mac-sign SIGN=$(CLAP_BUNDLE)
endif
	@echo "built $(CLAP_BUNDLE)"

# Loads it the way a host does and plays it. Compiling proves nothing about
# whether the event stream lands on the right frames or whether state survives
# a round trip; this does.
# The plugin/editor protocol, and the real editor process. Separate from
# clap-test because it needs the standalone binary and a display.
IPCTEST := bencsynth-ipc-test$(EXE)

ipc-test: $(IPCTEST) $(GUI)
	./$(IPCTEST) ./$(GUI)

$(IPCTEST): tools/ipc_test.cpp $(PLUGIN_SRC) $(PLUGIN_HDR) $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -Isrc/plugin -o $@ tools/ipc_test.cpp \
	    $(PLUGIN_SRC) $(CORE_LIB) $(CLAP_RT) $(IPC_X11)

CLAPHOST := bencsynth-clap-host$(EXE)

clap-test: clap $(CLAPHOST)
	./$(CLAPHOST) $(CLAP_BINARY)

$(CLAPHOST): tools/clap_host.cpp
	$(CXX) $(CXXFLAGS) -I$(CLAP_INCLUDE) -o $@ $< -ldl

# ------------------------------------------------------------------
# VST3 targets
# ------------------------------------------------------------------

# The VST3 SDK's submodules are fetched one at a time on purpose: the whole
# set pulls vstgui4, which is by far the largest piece of it and which a
# wrapper never touches.
vst3-fetch:
	mkdir -p vendor
	rm -rf $(CW_DIR) $(VST3_SDK_DIR)
	git clone -q --depth 1 --branch $(CW_VERSION) \
	    https://github.com/free-audio/clap-wrapper.git $(CW_DIR)
	git clone -q --depth 1 https://github.com/steinbergmedia/vst3sdk.git $(VST3_SDK_DIR)
	cd $(VST3_SDK_DIR) && git submodule update --init --depth 1 \
	    base pluginterfaces public.sdk cmake
	@echo "clap-wrapper $(CW_VERSION) and the VST3 SDK are in vendor/"

vst3:
ifeq ($(CLAP_FOUND),yes)
	@test -d $(CW_DIR) || { echo "  run 'make vst3-fetch' first."; false; }
	cmake -S cmake -B $(VST3_BUILD) -DCMAKE_BUILD_TYPE=Release \
	    -DCLAP_WRAPPER_DIR="$(CURDIR)/$(CW_DIR)" \
	    -DCLAP_SDK_ROOT="$(CURDIR)/vendor/clap" \
	    -DBS_VERSION="$(BS_VERSION)" \
	    -DVST3_SDK_ROOT="$(CURDIR)/$(VST3_SDK_DIR)"
	cmake --build $(VST3_BUILD) --target bencsynth_as_vst3 --config Release -j
	rm -rf $(VST3_BUNDLE)
	cp -r "$$(find $(VST3_BUILD) -name 'bencsynth.vst3' -maxdepth 3 | head -1)" $(VST3_BUNDLE)
	@echo "built $(VST3_BUNDLE)"
else
	@echo "  no CLAP headers - run 'make clap-fetch' first."
	@false
endif

# The VST3 is a shim: it finds bencsynth.clap in the standard CLAP directories
# at runtime, so the CLAP has to be installed too. Both, or the host loads a
# plugin that has nothing to play.
# ------------------------------------------------------------------
# AU - the same clap-wrapper, the other format Apple will load
#
# Logic and GarageBand do not load CLAP or VST3 and never will, so on macOS
# this is the difference between "works in REAPER" and "works". Like the VST3
# it is a shim: it finds bencsynth.clap at runtime rather than containing any
# of the synth, so the two can never drift.
# ------------------------------------------------------------------

au-fetch:
	@test "$(UNAME_S)" = "Darwin" || { echo "  AU is macOS only."; false; }
	mkdir -p vendor
	rm -rf $(AU_SDK_DIR)
	test -d $(CW_DIR) || git clone -q --depth 1 --branch $(CW_VERSION) \
	    https://github.com/free-audio/clap-wrapper.git $(CW_DIR)
	git clone -q --depth 1 --branch $(AU_SDK_TAG) \
	    https://github.com/apple/AudioUnitSDK.git $(AU_SDK_DIR)
	@echo "clap-wrapper $(CW_VERSION) and $(AU_SDK_TAG) are in vendor/"

au:
	@test "$(UNAME_S)" = "Darwin" || { echo "  AU is macOS only."; false; }
ifeq ($(CLAP_FOUND),yes)
	@test -d $(CW_DIR) || { echo "  run 'make au-fetch' first."; false; }
	@test -d $(AU_SDK_DIR) || { echo "  run 'make au-fetch' first."; false; }
	cmake -S cmake -B $(AU_BUILD) -DCMAKE_BUILD_TYPE=Release \
	    -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
	    -DCLAP_WRAPPER_DIR="$(CURDIR)/$(CW_DIR)" \
	    -DCLAP_SDK_ROOT="$(CURDIR)/vendor/clap" \
	    -DBS_VERSION="$(BS_VERSION)" \
	    -DAUDIOUNIT_SDK_ROOT="$(CURDIR)/$(AU_SDK_DIR)"
	cmake --build $(AU_BUILD) --target bencsynth_as_auv2 --config Release -j
	rm -rf $(AU_BUNDLE)
	cp -r "$$(find $(AU_BUILD) -name 'BENCsynth.component' -maxdepth 4 | head -1)" $(AU_BUNDLE)
	@# Ad-hoc, for the same reason the CLAP is: unsigned arm64 code does not
	@# load, and the message a host shows says nothing about signatures.
	codesign -s - -f --deep "$(AU_BUNDLE)"
	@echo "built $(AU_BUNDLE)"
else
	@echo "  no CLAP headers - run 'make clap-fetch' first."
	@false
endif

au-install: au clap-install
	mkdir -p "$(AU_INSTALL_DIR)"
	rm -rf "$(AU_INSTALL_DIR)/BENCsynth.component"
	cp -r $(AU_BUNDLE) "$(AU_INSTALL_DIR)/"
	@echo
	@echo "  installed to $(AU_INSTALL_DIR)/BENCsynth.component"
	@echo "  it loads bencsynth.clap from $(CLAP_INSTALL_DIR)"
	@echo "  Logic rescans on launch; auval -v aumu BNCS BNCO checks it now."

vst3-install: vst3 clap-install
	mkdir -p "$(VST3_INSTALL_DIR)"
	rm -rf "$(VST3_INSTALL_DIR)/bencsynth.vst3"
	cp -r $(VST3_BUNDLE) "$(VST3_INSTALL_DIR)/"
	@echo
	@echo "  installed to $(VST3_INSTALL_DIR)/bencsynth.vst3"
	@echo "  it loads bencsynth.clap from $(CLAP_INSTALL_DIR)"

# macOS will not load an unsigned bundle, and says "damaged" rather than
# anything about signatures. tools/macos-app.sh has done this for the program
# since the first release; the plugins never got the same treatment, which is
# exactly how they arrived reported as damaged.
#
# Ad-hoc (-s -) rather than a Developer ID: it satisfies the arm64 requirement
# that a binary be signed at all, and costs nothing. It does NOT clear the
# quarantine flag on a downloaded file - only notarisation does that - so a
# person who downloads a release still needs one xattr command, which the
# install notes give them.
# SIGN is what codesign is pointed at, which is not always the directory. A
# .clap is a real macOS bundle with a Contents/Info.plist and signs as one; an
# LV2 "bundle" is a plain directory of files, and codesign refuses it with
# "bundle format unrecognized, invalid, or unsuitable". There the thing to sign
# is the dylib inside.
mac-sign:
	@rm -rf $(dir $(SIGN))*.dSYM $(SIGN).dSYM $(SIGN)/Contents/MacOS/*.dSYM
	@if command -v codesign >/dev/null 2>&1; then \
	    codesign --force --sign - --timestamp=none "$(SIGN)" && \
	    codesign --verify --strict "$(SIGN)" && \
	    echo "  signed $(SIGN) (ad-hoc)"; \
	 else \
	    echo "  warning: no codesign - macOS will call this damaged"; \
	 fi

# One disk image with the plugins, the editor and an installer that clears
# the quarantine flag - which is the part a tarball cannot do, and the reason a
# downloaded plugin is reported as damaged even when it is signed.
macos-plugin-dmg:
	@test "$(UNAME_S)" = "Darwin" || { echo "  macOS only."; false; }
	./tools/macos-plugin-dmg.sh "bencsynth-plugins.dmg"

clap-install: clap
	mkdir -p "$(CLAP_INSTALL_DIR)"
	rm -rf "$(CLAP_INSTALL_DIR)/bencsynth.clap"
	cp -r $(CLAP_BUNDLE) "$(CLAP_INSTALL_DIR)/"
	@echo
	@echo "  installed to $(CLAP_INSTALL_DIR)/bencsynth.clap"
	@echo "  restart the host - it searches there by default."


%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

# windres resolves the path inside the .rc against its own working directory,
# so it is run from the project root and the .rc names the path from there.
src/gui/bencsynth.res.o: src/gui/bencsynth.rc assets/icon/bencsynth.ico
	windres $< -O coff -o $@


info:
	@echo "  raylib from = $(RL_FROM)"
	@echo "  RL_CFLAGS   = $(RL_CFLAGS)"
	@echo "  RL_LIBS     = $(RL_LIBS)"
	@echo "  RL_SYS      = $(RL_SYS)"
	@echo "  CXX         = $(CXX)"
	@if [ -z "$(RL_LIBS)" ]; then \
	  echo; \
	  echo "  No raylib found. Put one in vendor/raylib (include/ and lib/),"; \
	  echo "  pass RAYLIB=/prefix, or install the system package."; \
	  echo "  'make test' and 'make core' work without it."; \
	fi

clean:
	rm -f $(CORE_OBJ) $(GUI_OBJ) $(TEST_OBJ) $(RENDER_OBJ) \
	      $(CORE_OBJ:.o=.d) $(GUI_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(RENDER_OBJ:.o=.d) \
	      $(CORE_LIB) $(GUI) $(TEST) $(RENDER) $(LV2HOST) src/gui/*.res.o
	rm -rf $(LV2_BUNDLE) $(CLAP_BUNDLE) $(VST3_BUNDLE) $(VST3_BUILD)
	rm -f $(CLAPHOST) $(IPCTEST) $(LV2GEN)

-include $(CORE_OBJ:.o=.d) $(GUI_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(RENDER_OBJ:.o=.d)
