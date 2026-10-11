# Top-level build: final DFEngine.dll (proxy + hooklib + src hooks) into dist/.
CXX      := i686-w64-mingw32-g++
CXXFLAGS := -O2 -std=c++11 -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti -DDXHRFIX_HOOKS
LDFLAGS  := -shared -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup
LIBS     := -lbcrypt

SRCS := proxy/dfengine_proxy.cpp hooklib/hooklib.cpp src/dxhrfix_hooks.cpp src/dxhrfix_precreate.cpp src/dxhrfix_cursor.cpp \
        src/dxhrfix_profiler.cpp src/dxhrfix_prof_core.cpp
HDRS := hooklib/hooklib.h src/dxhrfix_hooks.h src/dxhrfix_internal.h src/dxhrfix_prof_core.h

all: dist/DFEngine.dll

dist/DFEngine.dll: $(SRCS) $(HDRS) proxy/DFEngine.def
	@mkdir -p dist
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRCS) proxy/DFEngine.def $(LIBS)

test: dist/DFEngine.dll
	$(MAKE) -C hooklib test
	$(MAKE) -C src/test test

harness: dist/DFEngine.dll
	dev/harness/test_launch.sh dist/DFEngine.dll 60

VERSION := 0.4.0
release: dist/DFEngine.dll
	rm -rf build/release && mkdir -p build/release/dxhrfix-$(VERSION)
	cp dist/DFEngine.dll dist/dxhrfix.ini dist/dxhrfix_syms.tsv dist/install.sh dist/install.bat dist/revert.sh dist/revert.bat dist/README.md build/release/dxhrfix-$(VERSION)/
	cd build/release && rm -f dxhrfix-$(VERSION).zip && zip -r -X dxhrfix-$(VERSION).zip dxhrfix-$(VERSION)
	@echo built build/release/dxhrfix-$(VERSION).zip

clean:
	rm -rf build
	$(MAKE) -C hooklib clean
	$(MAKE) -C src/test clean

.PHONY: all test harness release clean
