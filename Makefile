.PHONY: linux test clean

# ccache when available (the s2c CI image has it; a plain container may not).
CCACHE := $(if $(shell command -v ccache 2>/dev/null),ccache ,)

linux: build-linux/build.ninja
	@meson compile -C build-linux

build-linux/build.ninja: meson.build
	@CC='$(CCACHE)cc' CXX='$(CCACHE)c++' meson setup build-linux --buildtype=debugoptimized

test: build-linux/build.ninja
	@meson test -C build-linux --print-errorlogs

clean:
	@rm -rf build-linux
