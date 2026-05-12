/*
 * eqm_worker — Persistent Equihash worker process
 *
 * Two modes:
 *
 * 1. ONE-SHOT (legacy): ./eqm_worker <input_hex> <target_hex> <threads> <max_nonces>
 *    Runs once, prints JSON, exits.
 *
 * 2. PERSISTENT (new, default when no args): ./eqm_worker
 *    Reads commands from stdin, writes results to stdout.
 *    Commands (one per line, JSON):
 *      {"cmd":"solve","input":"<hex>","target":"<hex>","threads":4,"max_nonces":1000}
 *      {"cmd":"stop"}
 *    Responses:
 *      {"type":"progress","attempts":N,"hps":X}
 *      {"type":"result","found":true,"nonce":"<hex>","solution":"<hex>","attempts":N}
 *      {"type":"result","found":false,"attempts":N}
 *
 *    Benefits: no process spawn overhead per round, continuous mining.
 *
 * Compile:
 *   gcc -O3 -march=native -DWORKER_MODE -o eqm_worker eqm_worker.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#ifndef WORKER_MODE
#define WORKER_MODE
#endif
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

/* Extract string value from JSON-like line (simple parser) */
static int json_get_str(const char *line, const char *key, char *out, size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(line, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = end - p;
    if (len >= out_size) return -1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static int json_get_int(const char *line, const char *key, int *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(line, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ') p++;
    *out = atoi(p);
    return 0;
}

/* ─── One-shot mode ─── */
static int one_shot_mode(int argc, char *argv[]) {
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
        fprintf(stderr, "Invalid input hex\n");
        printf("{\"found\":false,\"attempts\":0}\n");
        return 1;
    }
    if (hex_to_bytes(argv[2], target, 32) != 0) {
        fprintf(stderr, "Invalid target hex\n");
        printf("{\"found\":false,\"attempts\":0}\n");
        return 1;
    }

    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;

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

/* ─── Persistent mode: read commands, stream results ─── */
static int persistent_mode(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* Unbuffered output */
    setvbuf(stdin, NULL, _IONBF, 0);

    fprintf(stderr, "[worker] Persistent mode ready\n");

    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        /* Strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (len == 0) continue;

        /* Check cmd */
        char cmd[32];
        if (json_get_str(line, "cmd", cmd, sizeof(cmd)) != 0) {
            printf("{\"type\":\"error\",\"message\":\"missing cmd\"}\n");
            continue;
        }

        if (strcmp(cmd, "stop") == 0) {
            fprintf(stderr, "[worker] Stop requested\n");
            break;
        }

        if (strcmp(cmd, "solve") != 0) {
            printf("{\"type\":\"error\",\"message\":\"unknown cmd\"}\n");
            continue;
        }

        /* Parse solve command */
        char input_hex[200], target_hex[100];
        int threads = 4, max_nonces = 1000;

        if (json_get_str(line, "input", input_hex, sizeof(input_hex)) != 0 ||
            json_get_str(line, "target", target_hex, sizeof(target_hex)) != 0) {
            printf("{\"type\":\"error\",\"message\":\"missing input or target\"}\n");
            continue;
        }

        json_get_int(line, "threads", &threads);
        json_get_int(line, "max_nonces", &max_nonces);

        uint8_t input[81], target[32];
        if (hex_to_bytes(input_hex, input, 81) != 0) {
            printf("{\"type\":\"error\",\"message\":\"invalid input hex\"}\n");
            continue;
        }
        if (hex_to_bytes(target_hex, target, 32) != 0) {
            printf("{\"type\":\"error\",\"message\":\"invalid target hex\"}\n");
            continue;
        }

        if (threads < 1) threads = 1;
        if (threads > 16) threads = 16;

        uint8_t nonce_out[32], solution_out[COMPRESSED_SIZE];
        int attempts = 0;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        int ret = equihash_solve_multi(input, target, threads, max_nonces,
                                        nonce_out, solution_out, &attempts);

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        double hps = attempts / elapsed;

        if (ret == 0) {
            char nonce_hex[65];
            char solution_hex[COMPRESSED_SIZE * 2 + 1];
            bytes_to_hex(nonce_out, 32, nonce_hex);
            bytes_to_hex(solution_out, COMPRESSED_SIZE, solution_hex);
            printf("{\"type\":\"result\",\"found\":true,\"nonce\":\"%s\",\"solution\":\"%s\",\"attempts\":%d,\"elapsed\":%.3f,\"hps\":%.1f}\n",
                   nonce_hex, solution_hex, attempts, elapsed, hps);
        } else {
            printf("{\"type\":\"result\",\"found\":false,\"attempts\":%d,\"elapsed\":%.3f,\"hps\":%.1f}\n",
                   attempts, elapsed, hps);
        }
        fflush(stdout);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    /* If args provided → one-shot mode (backwards compat) */
    if (argc > 1) {
        return one_shot_mode(argc, argv);
    }
    /* No args → persistent mode */
    return persistent_mode();
}
