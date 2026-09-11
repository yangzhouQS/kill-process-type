# Makefile - mingw32-make ???MinGW-w64?
CC      = gcc
WINDRES = windres
CFLAGS  = -municode -mwindows -O2 -Wall -Wextra -static
LIBS    = -lcomctl32 -lpsapi -liphlpapi
SRCS    = src/main.c src/gui.c src/views.c src/actions.c src/ai.c src/cli.c src/richtext.c src/config.c src/theme.c src/settings.c src/tray.c src/process.c src/net.c src/startup.c
OBJS    = build/main.o build/gui.o build/views.o build/actions.o build/ai.o build/cli.o build/richtext.o build/config.o build/theme.o build/settings.o build/tray.o build/process.o build/net.o build/startup.o build/app_res.o

all: build/kill-process-type.exe

build/kill-process-type.exe: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

build/app_res.o: res/app.rc res/app.ico res/manifest.xml | build
	$(WINDRES) res/app.rc -O coff -o $@

build/%.o: src/%.c src/common.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	if not exist build mkdir build

TESTFLAGS = -municode -O2 -Wall -static

test: build/test_process.exe build/test_net.exe build/test_startup.exe
	build\test_process.exe
	build\test_net.exe
	build\test_startup.exe

build/test_process.exe: tests/test_process.c src/process.c src/common.h src/process.h | build
	$(CC) $(TESTFLAGS) -o $@ tests\test_process.c src\process.c -lpsapi

build/test_net.exe: tests/test_net.c src/net.c src/common.h src/net.h | build
	$(CC) $(TESTFLAGS) -o $@ tests\test_net.c src\net.c -liphlpapi

build/test_startup.exe: tests/test_startup.c src/startup.c src/common.h src/startup.h | build
	$(CC) $(TESTFLAGS) -o $@ tests\test_startup.c src\startup.c -ladvapi32

clean:
	rm -rf build

.PHONY: all clean test
