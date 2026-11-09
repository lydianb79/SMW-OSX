# SMW Recomp — native macOS build (no Xcode project needed)
APP      := build/SMWRecomp.app
BIN      := $(APP)/Contents/MacOS/SMWRecomp
RES      := $(APP)/Contents/Resources
OBJDIR   := build/obj
CSRC     := $(wildcard src/core/*.c) src/app/audio_out.c
COBJ     := $(patsubst src/%.c,$(OBJDIR)/%.o,$(CSRC))
SWIFTSRC := $(wildcard src/app/*.swift)
ARCH     ?= $(shell uname -m)
TARGET   := $(ARCH)-apple-macos13.0
CFLAGS   := -O3 -std=c11 -Wall -Wno-unused-function -target $(TARGET)
SWFLAGS  := -O -target $(TARGET) -import-objc-header src/app/Bridging.h
FRAMEWORKS := -framework AppKit -framework Metal -framework MetalKit -framework AVFoundation \
              -framework GameController -framework SwiftUI -framework Carbon

.PHONY: all clean run test icon
all: $(APP)

$(OBJDIR)/%.o: src/%.c src/core/snes.h
	@mkdir -p $(dir $@)
	clang $(CFLAGS) -c $< -o $@

$(BIN): $(COBJ) $(SWIFTSRC) src/app/Bridging.h
	@mkdir -p $(dir $@)
	swiftc $(SWFLAGS) $(SWIFTSRC) $(COBJ) -o $@ $(FRAMEWORKS)

build/AppIcon.icns: tools/make_icon.swift
	@mkdir -p build
	rm -rf build/AppIcon.iconset
	swift tools/make_icon.swift build/AppIcon.iconset
	iconutil -c icns build/AppIcon.iconset -o $@

icon: build/AppIcon.icns

$(APP): $(BIN) build/AppIcon.icns Resources/Info.plist src/shaders/Shaders.metal
	@mkdir -p $(RES)
	cp Resources/Info.plist $(APP)/Contents/Info.plist
	cp build/AppIcon.icns $(RES)/AppIcon.icns
	cp src/shaders/Shaders.metal $(RES)/Shaders.metal
	@# precompile shaders when the Metal toolchain is installed; otherwise they compile at launch
	@if xcrun -sdk macosx metal -c src/shaders/Shaders.metal -o build/Shaders.air 2>/dev/null && \
	    xcrun -sdk macosx metallib build/Shaders.air -o $(RES)/default.metallib 2>/dev/null; then \
	    echo "metallib precompiled"; else rm -f $(RES)/default.metallib; echo "shaders will compile at runtime"; fi
	@if [ -f smw.smc ]; then cp smw.smc $(RES)/smw.smc; echo "bundled smw.smc"; fi
	codesign --force --deep -s - $(APP)
	@echo "Built $(APP)"

build/smwtest: $(wildcard src/core/*.c) src/core/snes.h tools/smwtest.c
	clang -O2 -std=c11 -o $@ src/core/*.c tools/smwtest.c

test: build/smwtest
	./build/smwtest smw.smc 600 build/test

run: all
	open $(APP)

clean:
	rm -rf build
