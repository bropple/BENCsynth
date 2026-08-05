# BENCsynth
#
#   make            the standalone synthesizer
#   make test       the core test, which needs no raylib and no sound card
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
GUI      := bencsynth

TEST_SRC := tests/test_core.cpp tests/test_patchfile.cpp
TEST_OBJ := $(TEST_SRC:.cpp=.o)
TEST     := bencsynth-test

RENDER_SRC := tools/render_wav.cpp
RENDER_OBJ := $(RENDER_SRC:.cpp=.o)
RENDER     := bencsynth-render

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

ifeq ($(OS),Windows_NT)
  # No console window behind the synthesizer when it is double-clicked.
  GUI_LINK := -mwindows
else
  GUI_LINK :=
endif

# ------------------------------------------------------------------

.PHONY: all core test clean info run render

all: $(GUI)

core: $(CORE_LIB)

$(CORE_LIB): $(CORE_OBJ)
	$(AR) rcs $@ $^

$(GUI): $(GUI_OBJ) $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $(GUI_LINK) -o $@ $(GUI_OBJ) $(CORE_LIB) $(LDLIBS_GUI)

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
	      $(CORE_LIB) $(GUI) $(TEST) $(RENDER)

-include $(CORE_OBJ:.o=.d) $(GUI_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(RENDER_OBJ:.o=.d)
