# BENCsynth
#
#   make            the standalone synthesizer
#   make test       the core test, which needs no raylib and no sound card
#   make render     a phrase through the default rack, written to a .wav
#   make icons      regenerate assets/icon from the star geometry in the code
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

GUI_SRC  := src/gui/main.cpp \
            src/gui/bs_gui.cpp \
            src/gui/bs_rope.cpp \
            src/gui/bs_rack.cpp \
            src/gui/bs_keyboard.cpp
GUI_OBJ  := $(GUI_SRC:.cpp=.o)
GUI      := bencsynth$(EXE)

TEST_SRC := tests/test_core.cpp tests/test_patchfile.cpp
TEST_OBJ := $(TEST_SRC:.cpp=.o)
TEST     := bencsynth-test$(EXE)

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
  GUI_LINK := -mwindows -static
else
  GUI_RES  :=
  GUI_LINK :=
endif


# ------------------------------------------------------------------

.PHONY: all core test clean info run render icons


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
	$(CXX) $(CXXFLAGS) $(GUI_LINK) -o $@ $(GUI_OBJ) $(GUI_RES) $(CORE_LIB) $(LDLIBS_GUI)

$(GUI_OBJ): CPPFLAGS += $(RL_CFLAGS) -Isrc/gui

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
	      $(CORE_LIB) $(GUI) $(TEST) $(RENDER) src/gui/*.res.o

-include $(CORE_OBJ:.o=.d) $(GUI_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(RENDER_OBJ:.o=.d)
