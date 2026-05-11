/*
 * BLAKE2b — Compact implementation with personalization (RFC 7693)
 * Single-header, no dependencies beyond stdint/string.
 */
#ifndef BLAKE2B_H
#define BLAKE2B_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define BLAKE2B_BLOCKBYTES  128
#define BLAKE2B_OUTBYTES    64
#define BLAKE2B_PERSONALBYTES 16

typedef struct {
    uint64_t h[8];
    uint64_t t[2];
    uint64_t f[2];
    uint8_t  buf[BLAKE2B_BLOCKBYTES];
    size_t   buflen;
    uint8_t  outlen;
} blake2b_state;

static const uint64_t blake2b_IV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static const uint8_t blake2b_sigma[12][16] = {
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    {14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},
    {11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4},
    {7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},
    {9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13},
    {2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},
    {12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11},
    {13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},
    {6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5},
    {10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    {14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3}
};

static inline uint64_t rotr64(uint64_t x, int n) {
    return (x >> n) | (x << (64 - n));
}

static inline uint64_t load64_le(const void *src) {
    uint64_t w;
    memcpy(&w, src, 8);
    return w;
}

static inline void store64_le(void *dst, uint64_t w) {
    memcpy(dst, &w, 8);
}

#define G(r,i,a,b,c,d) do { \
    a += b + m[blake2b_sigma[r][2*i]];   d = rotr64(d^a, 32); \
    c += d;                               b = rotr64(b^c, 24); \
    a += b + m[blake2b_sigma[r][2*i+1]]; d = rotr64(d^a, 16); \
    c += d;                               b = rotr64(b^c, 63); \
} while(0)

static inline void blake2b_compress(blake2b_state *S, const uint8_t block[128]) {
    uint64_t m[16], v[16];
    for (int i = 0; i < 16; i++) m[i] = load64_le(block + i*8);
    for (int i = 0; i < 8; i++) v[i] = S->h[i];
    v[8]=blake2b_IV[0]; v[9]=blake2b_IV[1]; v[10]=blake2b_IV[2]; v[11]=blake2b_IV[3];
    v[12]=blake2b_IV[4]^S->t[0]; v[13]=blake2b_IV[5]^S->t[1];
    v[14]=blake2b_IV[6]^S->f[0]; v[15]=blake2b_IV[7]^S->f[1];
    for (int r = 0; r < 12; r++) {
        G(r,0,v[0],v[4],v[8],v[12]);  G(r,1,v[1],v[5],v[9],v[13]);
        G(r,2,v[2],v[6],v[10],v[14]); G(r,3,v[3],v[7],v[11],v[15]);
        G(r,4,v[0],v[5],v[10],v[15]); G(r,5,v[1],v[6],v[11],v[12]);
        G(r,6,v[2],v[7],v[8],v[13]);  G(r,7,v[3],v[4],v[9],v[14]);
    }
    for (int i = 0; i < 8; i++) S->h[i] ^= v[i] ^ v[i+8];
}

static inline void blake2b_init_personal(blake2b_state *S, size_t outlen,
                                          const uint8_t personal[BLAKE2B_PERSONALBYTES]) {
    memset(S, 0, sizeof(*S));
    /* Build parameter block (all zeros except what we set) */
    uint8_t P[64];
    memset(P, 0, 64);
    P[0] = (uint8_t)outlen;  /* digest_length */
    P[2] = 1;               /* fanout */
    P[3] = 1;               /* depth */
    if (personal) memcpy(P + 48, personal, 16);
    for (int i = 0; i < 8; i++)
        S->h[i] = blake2b_IV[i] ^ load64_le(P + i*8);
    S->outlen = (uint8_t)outlen;
}

static inline void blake2b_update(blake2b_state *S, const void *in, size_t inlen) {
    const uint8_t *p = (const uint8_t *)in;
    while (inlen > 0) {
        size_t left = S->buflen;
        size_t fill = BLAKE2B_BLOCKBYTES - left;
        if (inlen > fill) {
            memcpy(S->buf + left, p, fill);
            S->t[0] += BLAKE2B_BLOCKBYTES;
            if (S->t[0] < BLAKE2B_BLOCKBYTES) S->t[1]++;
            blake2b_compress(S, S->buf);
            S->buflen = 0;
            p += fill; inlen -= fill;
        } else {
            memcpy(S->buf + S->buflen, p, inlen);
            S->buflen += inlen;
            return;
        }
    }
}

static inline void blake2b_final(blake2b_state *S, void *out) {
    S->t[0] += S->buflen;
    if (S->t[0] < S->buflen) S->t[1]++;
    S->f[0] = (uint64_t)-1;
    memset(S->buf + S->buflen, 0, BLAKE2B_BLOCKBYTES - S->buflen);
    blake2b_compress(S, S->buf);
    uint8_t buffer[64];
    for (int i = 0; i < 8; i++) store64_le(buffer + i*8, S->h[i]);
    memcpy(out, buffer, S->outlen);
}

#endif /* BLAKE2B_H */
