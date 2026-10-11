# Profiler helper unit tests: 32-bit MinGW build, run under host wine.
CXX      := i686-w64-mingw32-g++
CXXFLAGS := -O2 -std=c++11 -Wall -Wextra -static-libgcc -static-libstdc++
LDFLAGS  := -static

WINE        ?= /usr/bin/wine
SCRATCH     ?= $(abspath ../../build/test/proftest)
export WINEPREFIX := $(SCRATCH)/wineprefix
export WINEDEBUG  := -all

TEST_EXE := test_prof_core.exe

.PHONY: all test clean
all: $(TEST_EXE)

$(TEST_EXE): test_prof_core.cpp ../dxhrfix_prof_core.cpp ../dxhrfix_prof_core.h
	$(CXX) $(CXXFLAGS) test_prof_core.cpp ../dxhrfix_prof_core.cpp -o $@ $(LDFLAGS)

test: $(TEST_EXE)
	@mkdir -p $(WINEPREFIX)
	$(WINE) ./$(TEST_EXE)

clean:
	rm -f $(TEST_EXE)
