/*
 * Equihash (96, 5) Solver — Native C with pthreads
 * Optimized for x86_64 Linux (Ubuntu VPS).
 *
 * Wagner's algorithm: sort-based collision detection.
 * Multi-threaded: each thread tries independent nonces.
 *
 * Compile:
 *   gcc -O3 -march=native -pthread -shared -fPIC \
 *       -o libequihash.so equihash_solver.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "blake2b.h"

/* ─── Equihash (96, 5) Constants ─── */
#define N 96
#define K 5
#define COLLISION_BITS   (N / (K + 1))       /* 16 */
#define COLLISION_BYTES  2
#define HASH_LEN         60                   /* (512/N) * (N/8) = 5*12 */
#define INDICES_PER_HASH 5                    /* 512/N */
#define N_BYTES          12                   /* N/8 */
#define NUM_INITIAL      (1 << (COLLISION_BITS + 1))  /* 131072 */
#define SOLUTION_COUNT   (1 << K)             /* 32 */
#define BITS_PER_INDEX   (COLLISION_BITS + 1) /* 17 */
#define COMPRESSED_SIZE  68                   /* ceil(32 * 17 / 8) */
#define I_BLOCK_LEN      81

/* Max bucket size to avoid combinatorial explosion */
#define MAX_BUCKET_SIZE  12

/* ─── SHA-256 (for solution hash check) ─── */
/* Minimal SHA-256 for target comparison */
static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32-n)); }

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64], a,b,c,d,e,f,g,h,t1,t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|
               ((uint32_t)block[i*4+2]<<8)|block[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15],7)^rotr32(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = rotr32(w[i-2],17)^rotr32(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    a=state[0];b=state[1];c=state[2];d=state[3];
    e=state[4];f=state[5];g=state[6];h=state[7];
    for (int i = 0; i < 64; i++) {
        t1 = h+( rotr32(e,6)^rotr32(e,11)^rotr32(e,25) )+( (e&f)^((~e)&g) )+sha256_k[i]+w[i];
        t2 = ( rotr32(a,2)^rotr32(a,13)^rotr32(a,22) )+( (a&b)^(a&c)^(b&c) );
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
    state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint32_t state[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    uint8_t block[64];
    size_t i = 0;
    while (i + 64 <= len) { sha256_transform(state, data+i); i += 64; }
    size_t rem = len - i;
    memcpy(block, data+i, rem);
    block[rem] = 0x80;
    if (rem >= 56) {
        memset(block+rem+1, 0, 63-rem);
        sha256_transform(state, block);
        memset(block, 0, 56);
    } else {
        memset(block+rem+1, 0, 55-rem);
    }
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++) block[56+j] = (uint8_t)(bits >> (56-j*8));
    sha256_transform(state, block);
    for (int j = 0; j < 8; j++) {
        out[j*4]   = (uint8_t)(state[j]>>24);
        out[j*4+1] = (uint8_t)(state[j]>>16);
        out[j*4+2] = (uint8_t)(state[j]>>8);
        out[j*4+3] = (uint8_t)(state[j]);
    }
}

/* ─── Equihash Row ─── */
typedef struct {
    uint8_t  hash[N_BYTES];  /* 12 bytes, shrinks each round */
    uint32_t indices[SOLUTION_COUNT]; /* grows: 1,2,4,8,16,32 */
    uint8_t  num_indices;
    uint8_t  hash_len;       /* remaining hash bytes */
} eq_row;

/* ─── Generate leaf hash ─── */
static void generate_leaf(const blake2b_state *base_state, uint32_t index, uint8_t out[N_BYTES]) {
    blake2b_state s;
    memcpy(&s, base_state, sizeof(s));
    uint32_t idx_group = index / INDICES_PER_HASH;
    blake2b_update(&s, &idx_group, 4);
    uint8_t full[HASH_LEN];
    blake2b_final(&s, full);
    int offset = (index % INDICES_PER_HASH) * N_BYTES;
    memcpy(out, full + offset, N_BYTES);
}

/* ─── Comparison for sorting ─── */
static int row_compare(const void *a, const void *b) {
    const eq_row *ra = (const eq_row *)a;
    const eq_row *rb = (const eq_row *)b;
    return memcmp(ra->hash, rb->hash, COLLISION_BYTES);
}

/* ─── Check first COLLISION_BITS match ─── */
static inline int first_bits_match(const uint8_t *a, const uint8_t *b) {
    /* For COLLISION_BITS=16, just compare first 2 bytes */
    return a[0] == b[0] && a[1] == b[1];
}

/* ─── Check all indices distinct ─── */
static int all_distinct(const uint32_t *a, int na, const uint32_t *b, int nb) {
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nb; j++)
            if (a[i] == b[j]) return 0;
    return 1;
}

/* ─── XOR hashes ─── */
static inline void xor_hash(uint8_t *out, const uint8_t *a, const uint8_t *b, int len) {
    for (int i = 0; i < len; i++) out[i] = a[i] ^ b[i];
}

/* ─── Compress indices to bit-packed format ─── */
static void compress_indices(const uint32_t indices[SOLUTION_COUNT], uint8_t out[COMPRESSED_SIZE]) {
    memset(out, 0, COMPRESSED_SIZE);
    int pos = 0;
    for (int i = 0; i < SOLUTION_COUNT; i++) {
        uint32_t val = indices[i];
        for (int b = BITS_PER_INDEX - 1; b >= 0; b--) {
            int bit = (val >> b) & 1;
            out[pos / 8] |= (uint8_t)(bit << (7 - (pos % 8)));
            pos++;
        }
    }
}

/* ─── Hash under target (big-endian 256-bit) ─── */
static int hash_under_target(const uint8_t hash[32], const uint8_t target[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] < target[i]) return 1;
        if (hash[i] > target[i]) return 0;
    }
    return 0;
}

/* ─── Solution hash: SHA256(soln_indices || input) ─── */
static void compute_solution_hash(const uint8_t soln[COMPRESSED_SIZE],
                                   const uint8_t input[I_BLOCK_LEN],
                                   uint8_t out[32]) {
    uint8_t buf[COMPRESSED_SIZE + I_BLOCK_LEN];
    memcpy(buf, soln, COMPRESSED_SIZE);
    memcpy(buf + COMPRESSED_SIZE, input, I_BLOCK_LEN);
    sha256(buf, COMPRESSED_SIZE + I_BLOCK_LEN, out);
}

/* ─── Single-nonce solver ─── */
int equihash_solve_single(const uint8_t input[I_BLOCK_LEN],
                           const uint8_t nonce[32],
                           const uint8_t target[32],
                           uint8_t solution_out[COMPRESSED_SIZE]) {
    /* Build personalization: "ZcashPoW" + LE(n) + LE(k) */
    uint8_t personal[16];
    memset(personal, 0, 16);
    memcpy(personal, "ZcashPoW", 8);
    uint32_t n_le = N, k_le = K;
    memcpy(personal + 8, &n_le, 4);
    memcpy(personal + 12, &k_le, 4);

    /* Initialize base blake2b state with input + nonce */
    blake2b_state base;
    blake2b_init_personal(&base, HASH_LEN, personal);
    blake2b_update(&base, input, I_BLOCK_LEN);
    blake2b_update(&base, nonce, 32);

    /* Generate initial rows */
    eq_row *rows = (eq_row *)malloc(NUM_INITIAL * sizeof(eq_row));
    if (!rows) return 2;

    for (uint32_t i = 0; i < NUM_INITIAL; i++) {
        generate_leaf(&base, i, rows[i].hash);
        rows[i].indices[0] = i;
        rows[i].num_indices = 1;
        rows[i].hash_len = N_BYTES;
    }

    int num_rows = NUM_INITIAL;

    /* K Wagner rounds */
    for (int round = 0; round < K; round++) {
        /* Sort by first COLLISION_BYTES of hash */
        qsort(rows, num_rows, sizeof(eq_row), row_compare);

        /* Allocate output for this round */
        /* Worst case: num_rows * MAX_BUCKET_SIZE pairs, but we cap */
        int max_out = num_rows * 2; /* generous estimate */
        eq_row *next = (eq_row *)malloc(max_out * sizeof(eq_row));
        if (!next) { free(rows); return 2; }
        int next_count = 0;

        int i = 0;
        while (i < num_rows) {
            int j = i + 1;
            while (j < num_rows && first_bits_match(rows[i].hash, rows[j].hash))
                j++;

            /* Bucket [i, j) — limit to avoid explosion */
            int bucket_size = j - i;
            if (bucket_size > MAX_BUCKET_SIZE) bucket_size = MAX_BUCKET_SIZE;

            for (int ia = i; ia < i + bucket_size && next_count < max_out - 1; ia++) {
                for (int ib = ia + 1; ib < i + bucket_size && next_count < max_out - 1; ib++) {
                    int na = rows[ia].num_indices;
                    int nb = rows[ib].num_indices;

                    if (!all_distinct(rows[ia].indices, na, rows[ib].indices, nb))
                        continue;

                    eq_row *out = &next[next_count];

                    /* XOR hashes, trim collision bytes */
                    int new_hash_len = rows[ia].hash_len - COLLISION_BYTES;
                    if (new_hash_len < 0) new_hash_len = 0;

                    uint8_t xored[N_BYTES];
                    xor_hash(xored, rows[ia].hash, rows[ib].hash, rows[ia].hash_len);
                    memcpy(out->hash, xored + COLLISION_BYTES, new_hash_len);
                    out->hash_len = new_hash_len;

                    /* Merge indices canonically (smaller min first) */
                    if (rows[ia].indices[0] < rows[ib].indices[0]) {
                        memcpy(out->indices, rows[ia].indices, na * 4);
                        memcpy(out->indices + na, rows[ib].indices, nb * 4);
                    } else {
                        memcpy(out->indices, rows[ib].indices, nb * 4);
                        memcpy(out->indices + nb, rows[ia].indices, na * 4);
                    }
                    out->num_indices = na + nb;
                    next_count++;
                }
            }
            i = j;
        }

        free(rows);
        rows = next;
        num_rows = next_count;

        if (num_rows == 0) {
            free(rows);
            return 1; /* No solution */
        }
    }

    /* Check for valid solutions */
    int found = 1;
    for (int i = 0; i < num_rows; i++) {
        if (rows[i].num_indices != SOLUTION_COUNT) continue;

        /* Check residual hash is all zeros */
        int all_zero = 1;
        for (int j = 0; j < rows[i].hash_len; j++) {
            if (rows[i].hash[j] != 0) { all_zero = 0; break; }
        }
        if (!all_zero) continue;

        /* Compress and check against target */
        uint8_t compressed[COMPRESSED_SIZE];
        compress_indices(rows[i].indices, compressed);

        uint8_t sol_hash[32];
        compute_solution_hash(compressed, input, sol_hash);

        if (hash_under_target(sol_hash, target)) {
            memcpy(solution_out, compressed, COMPRESSED_SIZE);
            found = 0;
            break;
        }
    }

    free(rows);
    return found;
}

/* ─── Multi-threaded solver ─── */
typedef struct {
    const uint8_t *input;
    const uint8_t *target;
    int max_nonces;
    int thread_id;
    int num_threads;
    /* Output */
    volatile int *found_flag;
    uint8_t nonce_out[32];
    uint8_t solution_out[COMPRESSED_SIZE];
    int result;
    /* Stats */
    int attempts;
} worker_args;

static void fill_random(uint8_t *buf, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { fread(buf, 1, len, f); fclose(f); }
}

static void *worker_thread(void *arg) {
    worker_args *w = (worker_args *)arg;
    uint8_t nonce[32];
    w->attempts = 0;

    /* Seed with random + thread_id for uniqueness */
    fill_random(nonce, 32);
    nonce[0] ^= (uint8_t)w->thread_id;
    nonce[1] ^= (uint8_t)(w->thread_id >> 8);

    int per_thread = w->max_nonces / w->num_threads;

    for (int i = 0; i < per_thread; i++) {
        if (*w->found_flag) break;

        w->attempts++;
        int ret = equihash_solve_single(w->input, nonce, w->target, w->solution_out);

        if (ret == 0) {
            /* Found! */
            memcpy(w->nonce_out, nonce, 32);
            w->result = 0;
            *w->found_flag = 1;
            return NULL;
        }

        /* Increment nonce (treat as big counter) */
        for (int j = 31; j >= 0; j--) {
            if (++nonce[j] != 0) break;
        }
    }

    w->result = 1;
    return NULL;
}

/*
 * Public API: Multi-threaded Equihash solver
 *
 * Returns 0 on success, 1 if no solution found.
 * On success, nonce_out and solution_out are filled.
 */
int equihash_solve_multi(const uint8_t input[I_BLOCK_LEN],
                          const uint8_t target[32],
                          int num_threads,
                          int max_nonces,
                          uint8_t nonce_out[32],
                          uint8_t solution_out[COMPRESSED_SIZE],
                          int *attempts_out) {
    if (num_threads < 1) num_threads = 1;
    if (num_threads > 32) num_threads = 32;

    volatile int found_flag = 0;
    pthread_t *threads = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
    worker_args *args = (worker_args *)malloc(num_threads * sizeof(worker_args));

    for (int i = 0; i < num_threads; i++) {
        args[i].input = input;
        args[i].target = target;
        args[i].max_nonces = max_nonces;
        args[i].thread_id = i;
        args[i].num_threads = num_threads;
        args[i].found_flag = &found_flag;
        args[i].result = 1;
        args[i].attempts = 0;
        pthread_create(&threads[i], NULL, worker_thread, &args[i]);
    }

    int result = 1;
    int total_attempts = 0;

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
        total_attempts += args[i].attempts;
        if (args[i].result == 0 && result != 0) {
            memcpy(nonce_out, args[i].nonce_out, 32);
            memcpy(solution_out, args[i].solution_out, COMPRESSED_SIZE);
            result = 0;
        }
    }

    if (attempts_out) *attempts_out = total_attempts;

    free(threads);
    free(args);
    return result;
}

/* ─── Exported C API for Node.js FFI ─── */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * solve(input, target, threads, max_nonces, nonce_out, solution_out) -> int
 * Returns 0 on success.
 */
__attribute__((visibility("default")))
int eqm_solve(const uint8_t *input,
              const uint8_t *target,
              int threads,
              int max_nonces,
              uint8_t *nonce_out,
              uint8_t *solution_out,
              int *attempts_out) {
    return equihash_solve_multi(input, target, threads, max_nonces,
                                nonce_out, solution_out, attempts_out);
}

/* Version / info */
__attribute__((visibility("default")))
const char* eqm_version(void) {
    return "equihash-solver-native 1.0.0 (96,5) pthreads";
}

__attribute__((visibility("default")))
int eqm_solution_size(void) {
    return COMPRESSED_SIZE;
}

__attribute__((visibility("default")))
int eqm_input_size(void) {
    return I_BLOCK_LEN;
}

#ifdef __cplusplus
}
#endif

/* ─── Standalone test (compile without -shared) ─── */
#ifdef STANDALONE_TEST
int main(void) {
    printf("Equihash (96,5) Native Solver\n");
    printf("  NUM_INITIAL: %d\n", NUM_INITIAL);
    printf("  SOLUTION_COUNT: %d\n", SOLUTION_COUNT);
    printf("  COMPRESSED_SIZE: %d bytes\n", COMPRESSED_SIZE);
    printf("  Memory per solve: ~%d MB\n", (int)(NUM_INITIAL * sizeof(eq_row) / 1024 / 1024));

    /* Test with dummy data */
    uint8_t input[81] = {0};
    memcpy(input, "Equium-v1", 9);

    uint8_t target[32];
    memset(target, 0xFF, 32); /* Very easy target for testing */
    target[0] = 0x7F;

    uint8_t nonce[32], solution[COMPRESSED_SIZE];
    int attempts = 0;

    printf("\nSolving with 4 threads, max 100 nonces...\n");
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int ret = equihash_solve_multi(input, target, 4, 100, nonce, solution, &attempts);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

    if (ret == 0) {
        printf("✓ Solution found in %d attempts (%.2fs, %.1f H/s)\n",
               attempts, elapsed, attempts / elapsed);
    } else {
        printf("✗ No solution in %d attempts (%.2fs)\n", attempts, elapsed);
    }

    return 0;
}
#endif
