CC      ?= clang
SDK      = $(shell xcrun --show-sdk-path 2>/dev/null)
CFLAGS   = -O2 -Wall -Wextra -Wno-unused-parameter -I src/zjs \
           -isysroot $(SDK) -mmacosx-version-min=13.0
LDFLAGS  = -isysroot $(SDK) -lcupsimage -lcups -lpthread

BIN      = build/rastertozjs
OBJS     = build/rastertozjs.o build/foo2zjs.o build/jbig.o build/jbig_ar.o

VENDOR   = src/zjs/vendor

all: $(BIN)

build:
	mkdir -p build

build/rastertozjs.o: src/rastertozjs.c src/zjs/zjs_engine.h | build
	$(CC) $(CFLAGS) -c -o $@ src/rastertozjs.c

build/foo2zjs.o: $(VENDOR)/foo2zjs.c $(VENDOR)/zjs.h $(VENDOR)/jbig.h | build
	$(CC) $(CFLAGS) -c -o $@ $(VENDOR)/foo2zjs.c

build/jbig.o: $(VENDOR)/jbig.c $(VENDOR)/jbig.h $(VENDOR)/jbig_ar.h | build
	$(CC) $(CFLAGS) -c -o $@ $(VENDOR)/jbig.c

build/jbig_ar.o: $(VENDOR)/jbig_ar.c $(VENDOR)/jbig_ar.h | build
	$(CC) $(CFLAGS) -c -o $@ $(VENDOR)/jbig_ar.c

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

test: all
	./tests/run_tests.sh

clean:
	rm -rf build

.PHONY: all test clean
