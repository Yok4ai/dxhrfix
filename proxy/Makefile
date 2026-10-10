CXX      := i686-w64-mingw32-g++
CXXFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter -fno-exceptions -fno-rtti
LDFLAGS  := -shared -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup
LIBS     := -lbcrypt
WINE     ?= wine

all: DFEngine.dll

DFEngine.dll: dfengine_proxy.cpp DFEngine.def
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ dfengine_proxy.cpp DFEngine.def $(LIBS)

test/test_proxy.exe: test/test_proxy.cpp
	$(CXX) -O2 -Wall -static-libgcc -static-libstdc++ -o $@ $<

# Runs under host wine in a scratch dir (no DFEngine_orig.dll there, so the stub is used).
TESTDIR ?= /tmp/claude-1000/-mnt-Backup-SteamLibrary-steamapps-common-Deus-Ex-Human-Revolution-Director-s-Cut/1a6c9b22-e870-469d-a782-b4d3046827f6/scratchpad/proxy/unittest
test: DFEngine.dll test/test_proxy.exe
	rm -rf $(TESTDIR) && mkdir -p $(TESTDIR)
	cp DFEngine.dll test/test_proxy.exe $(TESTDIR)/
	cd $(TESTDIR) && WINEDEBUG=-all $(WINE) test_proxy.exe DFEngine.dll
	cat $(TESTDIR)/dxhrfix.log

exports: DFEngine.dll
	x86_64-w64-mingw32-objdump -p DFEngine.dll | sed -n '/\[Ordinal\/Name Pointer\]/,/^$$/p;/DLL Name/p'

clean:
	rm -f DFEngine.dll test/test_proxy.exe

.PHONY: all test exports clean
