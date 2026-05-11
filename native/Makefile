# Equihash (96,5) Native Solver — Makefile
# Target: Ubuntu x86_64 (DigitalOcean 4 vCPU)

CC = gcc
CFLAGS = -O3 -march=native -mtune=native -Wall -Wextra -fPIC
LDFLAGS = -shared -lpthread
SRC = equihash_solver.c
LIB = libequihash.so
TEST = equihash_test

.PHONY: all clean test install

all: $(LIB)

$(LIB): $(SRC) blake2b.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(LIB) $(SRC)
	@echo "✓ Built $(LIB)"
	@ls -lh $(LIB)

test: $(SRC) blake2b.h
	$(CC) $(CFLAGS) -DSTANDALONE_TEST -o $(TEST) $(SRC) -lpthread
	./$(TEST)

clean:
	rm -f $(LIB) $(TEST)

install: $(LIB)
	cp $(LIB) ../
	@echo "✓ Installed to project root"
