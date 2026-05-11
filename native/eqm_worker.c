/*
 * eqm_worker — Standalone Equihash worker process
 *
 * Called by Node.js miner. Reads input/target from argv (hex),
 * runs multi-threaded solver, outputs JSON result to stdout.
 *
 * Usage:
 *   ./eqm_worker <input_hex_162> <target_hex_64> <threads> <max_nonces>
 *
 * Output (JSON, one line):
 *   {"found":true,"nonce":"...hex...","solution":"...hex...","attempts":42}
 *
 * Compile:
 *   gcc -O3 -march=native -DWORKER_MODE -o eqm_worker equihash_solver.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Include the solver directly */
#define WORKER_MODE
#include "equihash_solver.c"

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len) {
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return -1;
    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte;
        if (sscanf(hex + i*2, "%2x", &byte) != 1) return -1;
        out[i] = (uint8_t)byte;
    }
    return 0;
}

static void bytes_to_hex(const uint8_t *data, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) {
        sprintf(out + i*2, "%02x", data[i]);
    }
    out[len*2] = '\0';
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <input_hex> <target_hex> <threads> <max_nonces>\n", argv[0]);
        printf("{\"found\":false,\"attempts\":0}\n");
        return 1;
    }

    uint8_t input[81];
    uint8_t target[32];
    int threads = atoi(argv[3]);
    int max_nonces = atoi(argv[4]);

    if (hex_to_bytes(argv[1], input, 81) != 0) {
        fprintf(stderr, "Invalid input hex (need 162 chars for 81 bytes)\n");
        printf("{\"found\":false,\"attempts\":0}\n");
        return 1;
    }

    if (hex_to_bytes(argv[2], target, 32) != 0) {
        fprintf(stderr, "Invalid target hex (need 64 chars for 32 bytes)\n");
        printf("{\"found\":false,\"attempts\":0}\n");
        return 1;
    }

    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;
    if (max_nonces < 1) max_nonces = 100;

    uint8_t nonce_out[32];
    uint8_t solution_out[COMPRESSED_SIZE];
    int attempts = 0;

    int ret = equihash_solve_multi(input, target, threads, max_nonces,
                                    nonce_out, solution_out, &attempts);

    if (ret == 0) {
        char nonce_hex[65];
        char solution_hex[COMPRESSED_SIZE * 2 + 1];
        bytes_to_hex(nonce_out, 32, nonce_hex);
        bytes_to_hex(solution_out, COMPRESSED_SIZE, solution_hex);
        printf("{\"found\":true,\"nonce\":\"%s\",\"solution\":\"%s\",\"attempts\":%d}\n",
               nonce_hex, solution_hex, attempts);
    } else {
        printf("{\"found\":false,\"attempts\":%d}\n", attempts);
    }

    return 0;
}
