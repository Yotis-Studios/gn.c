# gn.c - build and test
#
#   make              library (build/libgn.a)
#   make examples     build/game_loop and build/relay_server
#   make test         codec vectors, in-process client<->server tests, C++ wrapper
#   make test-live    against other implementations: gn.js server/clients (needs
#                     node; GNJS=../gn.js) and, if hemlock is installed, gn.hml
#                     (GNHML=../gn.hml)
#   make test32       the same, built -m32
#   make asan         everything under AddressSanitizer + UBSan
#   make fuzz         run both fuzzers for FUZZ_TIME seconds (clang)
#   make windows      cross-build Windows tests (WIN_CC, e.g. "zig cc -target x86-windows-gnu")
#   make test-windows run them under wine
#   make check        test + test-live + test32 + asan

CC ?= cc
CXX ?= c++
CFLAGS ?= -O2
WARN = -Wall -Wextra -Wpedantic -Werror
STD_C = -std=c99
STD_CXX = -std=c++98 -Wno-long-long
INC = -Iinclude
GNJS ?= ../gn.js
GNHML ?= ../gn.hml
FUZZ_TIME ?= 60
B = build

SRC = src/gn_codec.c src/gn_ws.c src/gn_client.c src/gn_server.c

.PHONY: all examples test test-live test32 asan fuzz windows test-windows check clean

all: $(B)/libgn.a

$(B):
	mkdir -p $(B)

$(B)/%.o: src/%.c include/gn.h src/gn_internal.h | $(B)
	$(CC) $(STD_C) $(WARN) $(CFLAGS) $(INC) -c $< -o $@

$(B)/libgn.a: $(B)/gn_codec.o $(B)/gn_ws.o $(B)/gn_client.o $(B)/gn_server.o
	$(AR) rcs $@ $^

$(B)/test_vectors: tests/test_vectors.c tests/json.h $(B)/libgn.a
	$(CC) $(STD_C) $(WARN) $(CFLAGS) $(INC) $< $(B)/libgn.a -lm -o $@

$(B)/test_client: tests/test_client.c $(B)/libgn.a
	$(CC) $(STD_C) $(WARN) $(CFLAGS) $(INC) $< $(B)/libgn.a -o $@

$(B)/test_server: tests/test_server.c $(B)/libgn.a
	$(CC) $(STD_C) $(WARN) $(CFLAGS) $(INC) $< $(B)/libgn.a -o $@

$(B)/server_peer: tests/server_peer.c $(B)/libgn.a
	$(CC) $(STD_C) $(WARN) $(CFLAGS) $(INC) $< $(B)/libgn.a -o $@

$(B)/test_cpp: tests/test_cpp.cpp include/gn.hpp $(B)/libgn.a
	$(CXX) $(STD_CXX) -pedantic -Wall -Wextra -Werror $(CFLAGS) $(INC) $< $(B)/libgn.a -o $@

$(B)/game_loop: examples/game_loop.cpp include/gn.hpp $(B)/libgn.a
	$(CXX) $(STD_CXX) -pedantic -Wall -Wextra -Werror $(CFLAGS) $(INC) $< $(B)/libgn.a -o $@

$(B)/relay_server: examples/relay_server.cpp include/gn.hpp $(B)/libgn.a
	$(CXX) $(STD_CXX) -pedantic -Wall -Wextra -Werror $(CFLAGS) $(INC) $< $(B)/libgn.a -o $@

examples: $(B)/game_loop $(B)/relay_server

# RUN prefixes every test binary (e.g. wine, setarch)
RUN ?=

test: $(B)/test_vectors $(B)/test_server $(B)/test_cpp examples
	$(RUN) $(B)/test_vectors tests/vectors/protocol.json
	$(RUN) $(B)/test_server
	$(RUN) $(B)/test_cpp

test-live: $(B)/test_client $(B)/server_peer
	RUNNER="$(RUN)" tests/run_live.sh $(B)/test_client $(GNJS)
	node tests/interop.js $(B)/server_peer $(GNJS) $(RUN)
	@if command -v hemlock >/dev/null && [ -d "$(GNHML)" ]; then \
		tests/run_interop_hml.sh $(B)/server_peer $(GNHML); \
	else echo "(skipping gn.hml interop: needs hemlock and $(GNHML))"; fi

test32:
	$(MAKE) B=build32 CFLAGS="$(CFLAGS) -m32" test test-live

# setarch -R: clang 14's sanitizer runtime intermittently segfaults before
# main() under the high mmap randomization of newer kernels (6.x).
NOASLR = setarch $$(uname -m) -R

asan:
	$(MAKE) B=build-asan CC=clang CXX=clang++ RUN="$(NOASLR)" \
		CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all" test test-live

# libFuzzer + ASan (see NOASLR above)
fuzz: | $(B)
	clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all $(INC) \
		src/gn_codec.c tests/fuzz/fuzz_decode.c -o $(B)/fuzz_decode
	clang -g -O1 -DGN_FUZZING -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all $(INC) \
		$(SRC) tests/fuzz/fuzz_client.c -o $(B)/fuzz_client
	clang -g -O1 -DGN_FUZZING -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all $(INC) \
		$(SRC) tests/fuzz/fuzz_server.c -o $(B)/fuzz_server
	mkdir -p $(B)/corpus_decode $(B)/corpus_client $(B)/corpus_server
	cd $(B) && $(NOASLR) ./fuzz_decode -max_total_time=$(FUZZ_TIME) -max_len=70000 corpus_decode
	cd $(B) && $(NOASLR) ./fuzz_client -max_total_time=$(FUZZ_TIME) -max_len=70000 corpus_client
	cd $(B) && $(NOASLR) ./fuzz_server -max_total_time=$(FUZZ_TIME) -max_len=70000 corpus_server

WIN_CC ?= zig cc -target x86-windows-gnu
WIN_CXX ?= zig c++ -target x86-windows-gnu
WB = build-win

windows:
	mkdir -p $(WB)
	$(WIN_CC) $(STD_C) $(WARN) $(INC) -c src/gn_codec.c -o $(WB)/gn_codec.o
	$(WIN_CC) $(STD_C) $(WARN) $(INC) -c src/gn_ws.c -o $(WB)/gn_ws.o
	$(WIN_CC) $(STD_C) $(WARN) $(INC) -c src/gn_client.c -o $(WB)/gn_client.o
	$(WIN_CC) $(STD_C) $(WARN) $(INC) -c src/gn_server.c -o $(WB)/gn_server.o
	$(WIN_CC) $(STD_C) $(WARN) $(INC) tests/test_vectors.c $(WB)/gn_codec.o -o $(WB)/test_vectors.exe
	$(WIN_CC) $(STD_C) $(WARN) $(INC) tests/test_client.c $(WB)/*.o -lws2_32 -o $(WB)/test_client.exe
	$(WIN_CC) $(STD_C) $(WARN) $(INC) tests/test_server.c $(WB)/*.o -lws2_32 -o $(WB)/test_server.exe
	$(WIN_CC) $(STD_C) $(WARN) $(INC) tests/server_peer.c $(WB)/*.o -lws2_32 -o $(WB)/server_peer.exe
	$(WIN_CXX) $(STD_CXX) -Wall -Wextra -Werror $(INC) tests/test_cpp.cpp $(WB)/*.o -lws2_32 -o $(WB)/test_cpp.exe

test-windows: windows
	WINEDEBUG=-all wine $(WB)/test_vectors.exe tests/vectors/protocol.json
	WINEDEBUG=-all wine $(WB)/test_server.exe
	WINEDEBUG=-all wine $(WB)/test_cpp.exe
	WINEDEBUG=-all RUNNER=wine WS_PORT=18541 tests/run_live.sh $(WB)/test_client.exe $(GNJS)
	WINEDEBUG=-all node tests/interop.js $(WB)/server_peer.exe $(GNJS) wine

check: test test-live test32 asan

clean:
	rm -rf build build32 build-asan build-win
