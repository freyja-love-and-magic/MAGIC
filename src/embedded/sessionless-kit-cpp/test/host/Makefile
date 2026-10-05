# Host tests: build the same library sources for macOS/Linux and check them against
# the shared JS-generated vectors. Run from this directory: `make test`.
CXX ?= c++
CC ?= cc
LIB := ../../libs/SessionlessKit/src
MAGIC := ../../libs/MagicCore/src
WAND := ../../libs/MagicWand/src
GATEWAY := ../../libs/MagicGateway/src
HOST := ../../libs/MagicHost/src
SECP := ../../libs/secp256k1-embedded
CFLAGS_SECP := -O2 -I$(SECP)/src -I$(SECP)/secp256k1 -I$(SECP)/secp256k1/include -I$(SECP)/secp256k1/src -w
VECTORS := ../../../test-vectors.json
MAGIC_VECTORS := ../../../magic-vectors.json

build/secp.o: $(SECP)/src/secp256k1_bundle.c
	@mkdir -p build
	$(CC) $(CFLAGS_SECP) -c $< -o $@

build/keccak.o: $(LIB)/keccak.c
	@mkdir -p build
	$(CC) -O2 -w -c $< -o build/keccak.o

build/test: test.cpp $(LIB)/SessionlessKit.cpp build/secp.o build/keccak.o
	$(CXX) -std=c++11 -O1 -g -Wall -Wextra -I$(LIB) -I$(SECP)/secp256k1/include -DVECTORS_PATH='"$(VECTORS)"' $^ -o $@

build/test_magic: test_magic.cpp $(MAGIC)/MagicCore.cpp $(LIB)/SessionlessKit.cpp build/secp.o build/keccak.o
	$(CXX) -std=c++11 -O1 -g -Wall -Wextra -I$(MAGIC) -I$(LIB) -I$(SECP)/secp256k1/include -DMAGIC_VECTORS_PATH='"$(MAGIC_VECTORS)"' $^ -o $@

build/test_wand_gateway: test_wand_gateway.cpp StrictLocalResolver.h $(MAGIC)/MagicCore.cpp $(WAND)/MagicWand.cpp $(GATEWAY)/MagicGateway.cpp $(LIB)/SessionlessKit.cpp build/secp.o build/keccak.o
	$(CXX) -std=c++11 -O1 -g -Wall -Wextra -I$(MAGIC) -I$(WAND) -I$(GATEWAY) -I$(LIB) -I$(SECP)/secp256k1/include test_wand_gateway.cpp $(MAGIC)/MagicCore.cpp $(WAND)/MagicWand.cpp $(GATEWAY)/MagicGateway.cpp $(LIB)/SessionlessKit.cpp build/secp.o build/keccak.o -o $@

test: build/test build/test_magic build/test_wand_gateway
	./build/test
	./build/test_magic
	./build/test_wand_gateway

clean:
	rm -rf build
.PHONY: test clean e2e

# End-to-end program (needs libcurl). Run via ../e2e/run.sh.
build/e2e: e2e_main.cpp $(MAGIC)/MagicCore.cpp $(WAND)/MagicWand.cpp $(GATEWAY)/MagicGateway.cpp $(HOST)/MagicCurlResolver.cpp $(LIB)/SessionlessKit.cpp build/secp.o build/keccak.o
	$(CXX) -std=c++11 -O1 -g -Wall -Wextra -I$(MAGIC) -I$(WAND) -I$(GATEWAY) -I$(HOST) -I$(LIB) -I$(SECP)/secp256k1/include $^ -lcurl -o $@

build/tls_test: tls_test.cpp $(MAGIC)/MagicCore.cpp $(HOST)/MagicCurlResolver.cpp $(LIB)/SessionlessKit.cpp build/secp.o build/keccak.o
	$(CXX) -std=c++11 -O1 -g -Wall -Wextra -I$(MAGIC) -I$(HOST) -I$(LIB) -I$(SECP)/secp256k1/include $^ -lcurl -o $@
