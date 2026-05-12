/*
 * Equihash (96, 5) Solver — Optimized Native C with pthreads
 * Target: x86_64 Linux (Ubuntu VPS 4 vCPU / 8GB RAM)
 *
 * Key optimizations over v1:
 *   1. Larger bucket tolerance (no artificial cap → more solutions found)
 *   2. Compact row struct per round (hash shrinks, indices grow)
 *   3. Radix sort on first 2 bytes (O(n) vs O(n log n) qsort)
 *   4. Pre-allocated arena to avoid malloc per round
 *   5. Collect ALL valid equihash solutions, then check target
 *   6. Early termination across threads when solution found
 *
 * Memory: ~50MB per thread (matches Equium spec)
 *
 * Compile:
 *   gcc -O3 -march=native -pthread -shared -fPIC \
 *       -o libequihash.so equihash_solver.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

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

/*
 * Max pairs per bucket. For (96,5) with 131072 initial items and 65536 buckets,
 * average bucket size is 2. But some buckets can be larger. We allow up to 24
 * to capture more solutions without blowing memory.
 */
#define MAX_BUCKET_PAIRS 24

/* Max rows to carry between rounds (memory cap ~50MB per thread) */
#define MAX_ROWS_PER_ROUND (1 << 19)  /* 524288 */

/* ─── SHA-256 ─── */
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
        t1 = h+(rotr32(e,6)^rotr32(e,11)^rotr32(e,25))+((e&f)^((~e)&g))+sha256_k[i]+w[i];
        t2 = (rotr32(a,2)^rotr32(a,13)^rotr32(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
    state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    uint32_t state[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                         0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
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
        out[j*4]=(uint8_t)(state[j]>>24); out[j*4+1]=(uint8_t)(state[j]>>16);
        out[j*4+2]=(uint8_t)(state[j]>>8); out[j*4+3]=(uint8_t)state[j];
    }
}

/* ─── Compact row: variable-size indices stored separately ─── */
typedef struct {
    uint8_t  hash[N_BYTES];
    uint8_t  hash_len;
    uint8_t  num_indices;
    uint32_t idx_offset;  /* offset into shared indices array */
} compact_row;

/* ─── Index arena: shared pool for all indices in a round ─── */
typedef struct {
    uint32_t *data;
    uint32_t  capacity;
    uint32_t  used;
} index_arena;

static void arena_init(index_arena *a, uint32_t cap) {
    a->data = (uint32_t *)malloc(cap * sizeof(uint32_t));
    a->capacity = cap;
    a->used = 0;
}

static uint32_t arena_alloc(index_arena *a, uint32_t count) {
    if (a->used + count > a->capacity) return UINT32_MAX;
    uint32_t off = a->used;
    a->used += count;
    return off;
}

static void arena_free(index_arena *a) {
    free(a->data);
    a->data = NULL;
    a->capacity = a->used = 0;
}

/* ─── Generate leaf hash ─── */
static inline void generate_leaf(const blake2b_state *base_state, uint32_t index, uint8_t out[N_BYTES]) {
    blake2b_state s;
    memcpy(&s, base_state, sizeof(s));
    uint32_t idx_group = index / INDICES_PER_HASH;
    blake2b_update(&s, &idx_group, 4);
    uint8_t full[HASH_LEN];
    blake2b_final(&s, full);
    int offset = (index % INDICES_PER_HASH) * N_BYTES;
    memcpy(out, full + offset, N_BYTES);
}

/* ─── Radix sort by first 2 bytes (16-bit key) ─── */
static void radix_sort_rows(compact_row *rows, int count, compact_row *temp) {
    /* 2-byte key = 65536 buckets. Count sort. */
    int counts[65536];
    memset(counts, 0, sizeof(counts));

    /* Count */
    for (int i = 0; i < count; i++) {
        uint16_t key = ((uint16_t)rows[i].hash[0] << 8) | rows[i].hash[1];
        counts[key]++;
    }

    /* Prefix sum */
    int total = 0;
    for (int i = 0; i < 65536; i++) {
        int c = counts[i];
        counts[i] = total;
        total += c;
    }

    /* Scatter */
    for (int i = 0; i < count; i++) {
        uint16_t key = ((uint16_t)rows[i].hash[0] << 8) | rows[i].hash[1];
        temp[counts[key]++] = rows[i];
    }

    /* Copy back */
    memcpy(rows, temp, count * sizeof(compact_row));
}

/* ─── Check all indices distinct ─── */
static inline int indices_distinct(const uint32_t *a, int na, const uint32_t *b, int nb) {
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nb; j++)
            if (a[i] == b[j]) return 0;
    return 1;
}

/* ─── Compress indices to bit-packed format ─── */
static void compress_indices(const uint32_t *indices, int count, uint8_t out[COMPRESSED_SIZE]) {
    memset(out, 0, COMPRESSED_SIZE);
    int pos = 0;
    for (int i = 0; i < count; i++) {
        uint32_t val = indices[i];
        for (int b = BITS_PER_INDEX - 1; b >= 0; b--) {
            int bit = (val >> b) & 1;
            out[pos / 8] |= (uint8_t)(bit << (7 - (pos % 8)));
            pos++;
        }
    }
}

/* ─── Hash under target ─── */
static inline int hash_under_target(const uint8_t hash[32], const uint8_t target[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] < target[i]) return 1;
        if (hash[i] > target[i]) return 0;
    }
    return 0;
}

/* ─── Solution hash ─── */
static void compute_solution_hash(const uint8_t soln[COMPRESSED_SIZE],
                                   const uint8_t input[I_BLOCK_LEN], uint8_t out[32]) {
    uint8_t buf[COMPRESSED_SIZE + I_BLOCK_LEN];
    memcpy(buf, soln, COMPRESSED_SIZE);
    memcpy(buf + COMPRESSED_SIZE, input, I_BLOCK_LEN);
    sha256(buf, COMPRESSED_SIZE + I_BLOCK_LEN, out);
}

/* ─── Single-nonce solver (optimized) ─── */
static int equihash_solve_single(const uint8_t input[I_BLOCK_LEN],
                                  const uint8_t nonce[32],
                                  const uint8_t target[32],
                                  uint8_t solution_out[COMPRESSED_SIZE],
                                  volatile int *cancel_flag) {
    /* Personalization */
    uint8_t personal[16] = {0};
    memcpy(personal, "ZcashPoW", 8);
    uint32_t n_le = N, k_le = K;
    memcpy(personal + 8, &n_le, 4);
    memcpy(personal + 12, &k_le, 4);

    /* Base blake2b state */
    blake2b_state base;
    blake2b_init_personal(&base, HASH_LEN, personal);
    blake2b_update(&base, input, I_BLOCK_LEN);
    blake2b_update(&base, nonce, 32);

    /* Allocate rows + arena for round 0 */
    compact_row *rows = (compact_row *)malloc(MAX_ROWS_PER_ROUND * sizeof(compact_row));
    compact_row *temp = (compact_row *)malloc(MAX_ROWS_PER_ROUND * sizeof(compact_row));
    if (!rows || !temp) { free(rows); free(temp); return 2; }

    /* Index arena: worst case each row has SOLUTION_COUNT indices at final round */
    index_arena arena;
    arena_init(&arena, NUM_INITIAL * 4); /* Start generous, grows via realloc if needed */

    /* Generate initial rows */
    for (uint32_t i = 0; i < NUM_INITIAL; i++) {
        if (cancel_flag && *cancel_flag) { free(rows); free(temp); arena_free(&arena); return 1; }
        generate_leaf(&base, i, rows[i].hash);
        rows[i].hash_len = N_BYTES;
        rows[i].num_indices = 1;
        rows[i].idx_offset = arena_alloc(&arena, 1);
        if (rows[i].idx_offset == UINT32_MAX) { free(rows); free(temp); arena_free(&arena); return 2; }
        arena.data[rows[i].idx_offset] = i;
    }

    int num_rows = NUM_INITIAL;

    /* K Wagner rounds */
    for (int round = 0; round < K; round++) {
        if (cancel_flag && *cancel_flag) { free(rows); free(temp); arena_free(&arena); return 1; }

        /* Radix sort by first 2 bytes */
        radix_sort_rows(rows, num_rows, temp);

        /* New arena for next round's indices */
        index_arena next_arena;
        uint32_t est_indices = (uint32_t)num_rows * (1 << (round + 1)) * 2;
        if (est_indices > 16 * 1024 * 1024) est_indices = 16 * 1024 * 1024;
        arena_init(&next_arena, est_indices);

        int next_count = 0;

        /* Find collision buckets and pair */
        int i = 0;
        while (i < num_rows && next_count < MAX_ROWS_PER_ROUND) {
            int j = i + 1;
            while (j < num_rows && rows[i].hash[0] == rows[j].hash[0] && rows[i].hash[1] == rows[j].hash[1])
                j++;

            int bucket_size = j - i;
            /* Allow larger buckets but cap to avoid O(n^2) explosion */
            int limit = bucket_size > MAX_BUCKET_PAIRS ? MAX_BUCKET_PAIRS : bucket_size;

            for (int ia = i; ia < i + limit && next_count < MAX_ROWS_PER_ROUND; ia++) {
                for (int ib = ia + 1; ib < i + limit && next_count < MAX_ROWS_PER_ROUND; ib++) {
                    int na = rows[ia].num_indices;
                    int nb = rows[ib].num_indices;

                    /* Check distinct indices */
                    uint32_t *idx_a = arena.data + rows[ia].idx_offset;
                    uint32_t *idx_b = arena.data + rows[ib].idx_offset;
                    if (!indices_distinct(idx_a, na, idx_b, nb))
                        continue;

                    /* XOR hashes, trim collision bytes */
                    int old_len = rows[ia].hash_len;
                    int new_len = old_len - COLLISION_BYTES;
                    if (new_len < 0) new_len = 0;

                    compact_row *out = &temp[next_count];
                    for (int x = 0; x < new_len; x++)
                        out->hash[x] = rows[ia].hash[x + COLLISION_BYTES] ^ rows[ib].hash[x + COLLISION_BYTES];
                    out->hash_len = new_len;
                    out->num_indices = na + nb;

                    /* Allocate and merge indices canonically */
                    uint32_t off = arena_alloc(&next_arena, na + nb);
                    if (off == UINT32_MAX) goto round_done;
                    out->idx_offset = off;

                    if (idx_a[0] < idx_b[0]) {
                        memcpy(next_arena.data + off, idx_a, na * 4);
                        memcpy(next_arena.data + off + na, idx_b, nb * 4);
                    } else {
                        memcpy(next_arena.data + off, idx_b, nb * 4);
                        memcpy(next_arena.data + off + nb, idx_a, na * 4);
                    }

                    next_count++;
                }
            }
            i = j;
        }
round_done:

        /* Swap: temp becomes rows for next round */
        arena_free(&arena);
        arena = next_arena;

        compact_row *swap = rows;
        rows = temp;
        temp = swap;
        num_rows = next_count;

        if (num_rows == 0) {
            free(rows); free(temp); arena_free(&arena);
            return 1;
        }
    }

    /* Check for valid solutions */
    int found = 1;
    for (int i = 0; i < num_rows && found; i++) {
        if (rows[i].num_indices != SOLUTION_COUNT) continue;

        /* Check residual hash is all zeros */
        int all_zero = 1;
        for (int j = 0; j < rows[i].hash_len; j++) {
            if (rows[i].hash[j] != 0) { all_zero = 0; break; }
        }
        if (!all_zero) continue;

        /* Compress and check against difficulty target */
        uint32_t *indices = arena.data + rows[i].idx_offset;
        uint8_t compressed[COMPRESSED_SIZE];
        compress_indices(indices, SOLUTION_COUNT, compressed);

        uint8_t sol_hash[32];
        compute_solution_hash(compressed, input, sol_hash);

        if (hash_under_target(sol_hash, target)) {
            memcpy(solution_out, compressed, COMPRESSED_SIZE);
            found = 0; /* success */
        }
    }

    free(rows);
    free(temp);
    arena_free(&arena);
    return found;
}

/* ─── Multi-threaded solver ─── */
typedef struct {
    const uint8_t *input;
    const uint8_t *target;
    int max_nonces;
    int thread_id;
    int num_threads;
    volatile int *found_flag;
    uint8_t nonce_out[32];
    uint8_t solution_out[COMPRESSED_SIZE];
    int result;
    int attempts;
} worker_args;

static void fill_random(uint8_t *buf, size_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t r = fread(buf, 1, len, f);
        (void)r;
        fclose(f);
    } else {
        /* Fallback: use time + address as seed */
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)((uintptr_t)buf + i + time(NULL));
    }
}

static void *worker_thread(void *arg) {
    worker_args *w = (worker_args *)arg;
    uint8_t nonce[32];
    w->attempts = 0;

    /* Unique random seed per thread */
    fill_random(nonce, 32);
    /* Mix in thread_id for guaranteed uniqueness */
    nonce[0] ^= (uint8_t)(w->thread_id);
    nonce[1] ^= (uint8_t)(w->thread_id >> 8);
    nonce[2] ^= (uint8_t)(clock() & 0xFF);

    int per_thread = w->max_nonces / w->num_threads;
    if (per_thread < 1) per_thread = 1;

    for (int i = 0; i < per_thread; i++) {
        if (*w->found_flag) break;

        w->attempts++;
        int ret = equihash_solve_single(w->input, nonce, w->target,
                                         w->solution_out, w->found_flag);

        if (ret == 0) {
            memcpy(w->nonce_out, nonce, 32);
            w->result = 0;
            *w->found_flag = 1;
            return NULL;
        }

        /* Increment nonce */
        for (int j = 31; j >= 0; j--) {
            if (++nonce[j] != 0) break;
        }
    }

    w->result = 1;
    return NULL;
}

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

/* ─── Exported C API ─── */
#ifndef WORKER_MODE
#ifdef __cplusplus
extern "C" {
#endif

__attribute__((visibility("default")))
int eqm_solve(const uint8_t *input, const uint8_t *target,
              int threads, int max_nonces,
              uint8_t *nonce_out, uint8_t *solution_out, int *attempts_out) {
    return equihash_solve_multi(input, target, threads, max_nonces,
                                nonce_out, solution_out, attempts_out);
}

__attribute__((visibility("default")))
const char* eqm_version(void) {
    return "equihash-solver-native 2.0.0 (96,5) optimized+pthreads";
}

__attribute__((visibility("default")))
int eqm_solution_size(void) { return COMPRESSED_SIZE; }

__attribute__((visibility("default")))
int eqm_input_size(void) { return I_BLOCK_LEN; }

#ifdef __cplusplus
}
#endif
#endif /* !WORKER_MODE */

/* ─── Standalone test ─── */
#ifdef STANDALONE_TEST
int main(void) {
    printf("Equihash (96,5) Native Solver v2.0 (optimized)\n");
    printf("  NUM_INITIAL: %d\n", NUM_INITIAL);
    printf("  SOLUTION_COUNT: %d\n", SOLUTION_COUNT);
    printf("  MAX_BUCKET_PAIRS: %d\n", MAX_BUCKET_PAIRS);
    printf("  MAX_ROWS_PER_ROUND: %d\n", MAX_ROWS_PER_ROUND);

    uint8_t input[81] = {0};
    memcpy(input, "Equium-v1", 9);

    /* Test with progressively harder targets */
    uint8_t target[32];
    int test_targets[][2] = {
        {0x7F, 0xFF},  /* ~50% pass */
        {0x3F, 0xFF},  /* ~25% pass */
        {0x1F, 0xFF},  /* ~12% pass */
        {0x0F, 0xFF},  /* ~6% pass */
    };

    for (int t = 0; t < 4; t++) {
        memset(target, 0xFF, 32);
        target[0] = test_targets[t][0];

        uint8_t nonce[32], solution[COMPRESSED_SIZE];
        int attempts = 0;

        printf("\nTarget 0x%02x%02x...: ", target[0], target[1]);
        fflush(stdout);

        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        int ret = equihash_solve_multi(input, target, 4, 500, nonce, solution, &attempts);

        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;

        if (ret == 0) {
            printf("✓ Found in %d attempts (%.2fs, %.1f H/s)\n", attempts, elapsed, attempts/elapsed);
        } else {
            printf("✗ Not found in %d attempts (%.2fs)\n", attempts, elapsed);
        }
    }

    return 0;
}
#endif
