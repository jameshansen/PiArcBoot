# PiArcBoot. Needs gcc, SDL2 and zlib (libsdl2-dev and zlib1g-dev on Raspberry Pi OS and Debian),
# and ROOGUI, the library that draws its windows: in a folder called roogui here, or ROOGUI
# beside this one.
#   make            build/piarcboot, and build/libroogui.so beside it
#   make check      the self-tests
# VERSION is what the About tab shows: a release's comes from its tag (v1.0.0 gives 1.0.0), and
# otherwise it is git's name for the commit, or "dev" outside git.
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null | sed 's/^v//')
CC ?= gcc
OPT ?= -O2
ARCEM_OPT ?= -O3
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS := $(shell sdl2-config --libs 2>/dev/null)

ROOGUI ?= $(firstword $(wildcard roogui ../ROOGUI))

# The program finds libroogui.so in the folder it is in itself
LIBROOGUI := build/libroogui.so
LINK := -Wl,-rpath,'$$ORIGIN'
ROOGUI_MAKE :=

# On a Mac: SDL2 is the framework from SDL's own release, in the folder SDL_FRAMEWORKS, and goes
# in build/ beside the program. ARCHS="arm64 x86_64" makes one program for Apple silicon and Intel
# Macs. ROOGUI is libroogui.dylib, found beside the program or in the application's Frameworks
# folder, and the menu bar's file is Objective-C.
ifeq ($(shell uname -s),Darwin)
SDL_FRAMEWORKS ?= ../deps
ARCHS ?=
MACOS_MIN ?= 10.13
override CC := $(CC) $(foreach a,$(ARCHS),-arch $(a)) -mmacosx-version-min=$(MACOS_MIN) -Wno-unknown-warning-option
SDL_CFLAGS := -I$(abspath $(SDL_FRAMEWORKS))/SDL2.framework/Headers -F$(abspath $(SDL_FRAMEWORKS))
SDL_LIBS := -F$(abspath $(SDL_FRAMEWORKS)) -framework SDL2
LIBROOGUI := build/libroogui.dylib
LINK := -Wl,-rpath,@executable_path -Wl,-rpath,@executable_path/../Frameworks -framework Cocoa
ROOGUI_MAKE := CC="$(CC)" OPT="$(OPT)" SDL_CFLAGS="$(SDL_CFLAGS)" SDL_LIBS="$(SDL_LIBS)"
build/src/menubar.o: OPT += -x objective-c -fobjc-arc
build/piarcboot: build/SDL2.framework
build/SDL2.framework: $(SDL_FRAMEWORKS)/SDL2.framework
	@mkdir -p build
	rm -rf $@
	cp -R $< $@
endif

# The machine is ArcEm's, in arcem/ as ArcEm has its files. armemudec.c and armemuinstr.c are
# part of armemu.c, arch/fastmap.c is part of the files that use arch/fastmap.h, and
# arch/stddisplaydev.c is part of src/screen.c.
ARCEM := arcem/armcopro.c arcem/armemu.c arcem/arminit.c arcem/armsupp.c arcem/eventq.c arcem/hostfs.c \
	arcem/arch/archio.c arcem/arch/armarc.c arcem/arch/cp15.c arcem/arch/displaydev.c arcem/arch/extnrom.c \
	arcem/arch/filecommon.c arcem/arch/fileunix.c arcem/arch/i2c.c arcem/arch/keyboard.c arcem/arch/newsound.c
ARCEM_DEFS := -DSOUND_SUPPORT -DHOSTFS_SUPPORT -DEXTNROM_SUPPORT
ARCEM_H := $(wildcard arcem/*.h arcem/arch/*.h) arcem/armemudec.c arcem/armemuinstr.c arcem/arch/fastmap.c \
	arcem/arch/stddisplaydev.c
SRC := $(wildcard src/*.c)
OBJ := $(patsubst %.c,build/%.o,$(ARCEM) $(SRC))

build/piarcboot: $(OBJ) $(LIBROOGUI)
	$(CC) -o $@ $(OBJ) -Lbuild -lroogui $(LINK) $(SDL_LIBS) -lz -lm -ldl

# ROOGUI is a library of its own, built by its own Makefile, under its own licence
$(LIBROOGUI): $(wildcard $(ROOGUI)/src/*.c $(ROOGUI)/src/*.h)
	$(MAKE) -C $(ROOGUI) build/libroogui.so $(ROOGUI_MAKE)
	@mkdir -p build
	cp $(ROOGUI)/build/libroogui.so $@
	$(if $(filter %.dylib,$@),install_name_tool -id @rpath/libroogui.dylib $@)
	cp $(ROOGUI)/LICENSE build/LICENSE-ROOGUI

build/arcem/%.o: arcem/%.c $(ARCEM_H)
	@mkdir -p $(dir $@)
	$(CC) $(ARCEM_OPT) -std=gnu17 -Wall -W -Wno-unused-parameter $(ARCEM_DEFS) -c $< -o $@

# HostFS reads and writes the computer's files with glibc's 64-bit file calls
build/arcem/hostfs.o: ARCEM_DEFS += -D_LARGEFILE64_SOURCE

# The picture's code is ArcEm's display driver, as fast as the machine
build/src/screen.o: OPT := $(ARCEM_OPT)

# launcher.o, setup.o and main.o aren't rebuilt when only VERSION changes; a release builds from clean
build/src/launcher.o build/src/setup.o build/src/main.o: OPT += $(if $(VERSION),-DPIARCBOOT_VERSION='"$(VERSION)"')

# The front of the installer, which unpacks the PiArcBoot folder packed behind it (tools/mkinstaller.py)
build/stub: tools/stub.c
	@mkdir -p build
	$(CC) -O2 -Wall -o $@ $<

build/src/%.o: src/%.c $(wildcard src/*.h) $(ARCEM_H) $(ROOGUI)/src/roogui.h
	@mkdir -p build/src
	$(CC) $(OPT) -std=gnu17 -Wall -Wno-format-truncation $(ARCEM_DEFS) -I$(ROOGUI)/src $(SDL_CFLAGS) -c $< -o $@

# The self-tests: the disc image code, the icon and PNG code, and the game table
check: build/piarcboot
	PIARCBOOT_HOME=build/check-home ./build/piarcboot check

clean:
	rm -rf build

.PHONY: check clean
