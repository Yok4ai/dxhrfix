# hooklib: 32-bit MinGW build + wine-run unit tests.
CXX      := i686-w64-mingw32-g++
CXXFLAGS := -O2 -std=c++11 -Wall -Wextra -static-libgcc -static-libstdc++
LDFLAGS  := -static

WINE        ?= /usr/bin/wine
SCRATCH     ?= $(abspath ../build/test/hooklib)
export WINEPREFIX := $(SCRATCH)/wineprefix
export WINEDEBUG  := -all

TEST_EXE := test_hooklib.exe

.PHONY: all test clean
all: $(TEST_EXE)

$(TEST_EXE): test_hooklib.cpp hooklib.cpp hooklib.h
	$(CXX) $(CXXFLAGS) test_hooklib.cpp hooklib.cpp -o $@ $(LDFLAGS)

test: $(TEST_EXE)
	@mkdir -p $(WINEPREFIX)
	$(WINE) ./$(TEST_EXE)

clean:
	rm -f $(TEST_EXE)
