/* axsha256.h — shared SHA-256 for userspace setup tools.
   Same algorithm as the kernel verifier (security.c) and the CLI OOBE
   (oobe.c): password hashes written here must verify there. Header-only,
   included by the GUI setup client; the CLI variant keeps its own copy. */

#ifndef AXSHA256_H
#define AXSHA256_H

#include <stdint.h>
#include <stddef.h>
#include "string.h"
#include "stdlib.h"

static uint32_t axsha_rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32 - n));
}

static const uint32_t AXSHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void axsha256(const uint8_t *msg, size_t len, uint8_t out[32])
{
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t total = len + 1;
    uint64_t bitlen = (uint64_t)len * 8;
    size_t padded = total + (64 - (total % 64)) % 64;
    if ((padded % 64) == 0 && (total % 64) > 56) padded += 64;
    else if (total % 64 == 0) padded = total + 64;

    uint8_t *buf = malloc(padded ? padded : 64);
    if (!buf) { memset(out, 0, 32); return; }
    size_t i;
    for (i = 0; i < len; i++) buf[i] = msg[i];
    buf[i++] = 0x80;
    while (i < padded - 8) buf[i++] = 0;
    for (int j = 0; j < 8; j++)
        buf[padded - 1 - j] = (uint8_t)(bitlen >> (8 * j));

    for (size_t off = 0; off < padded; off += 64)
    {
        uint32_t w[64];
        for (int t = 0; t < 16; t++)
            w[t] = ((uint32_t)buf[off + t*4] << 24) |
                   ((uint32_t)buf[off + t*4+1] << 16) |
                   ((uint32_t)buf[off + t*4+2] << 8) |
                   ((uint32_t)buf[off + t*4+3]);
        for (int t = 16; t < 64; t++)
            w[t] = (axsha_rotr(w[t-2],17) ^ axsha_rotr(w[t-2],19) ^ (w[t-2]>>10)) + w[t-7] +
                   (axsha_rotr(w[t-15],7) ^ axsha_rotr(w[t-15],18) ^ (w[t-15]>>3)) + w[t-16];

        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int t = 0; t < 64; t++)
        {
            uint32_t S1 = axsha_rotr(e,6)^axsha_rotr(e,11)^axsha_rotr(e,25);
            uint32_t ch = (e&f)^((~e)&g);
            uint32_t t1 = hh + S1 + ch + AXSHA_K[t] + w[t];
            uint32_t S0 = axsha_rotr(a,2)^axsha_rotr(a,13)^axsha_rotr(a,22);
            uint32_t maj = (a&b)^(a&c)^(b&c);
            uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    free(buf);
    for (int i = 0; i < 8; i++)
    {
        out[i*4]   = (uint8_t)(h[i] >> 24);
        out[i*4+1] = (uint8_t)(h[i] >> 16);
        out[i*4+2] = (uint8_t)(h[i] >> 8);
        out[i*4+3] = (uint8_t)(h[i]);
    }
}

static void axsha256_hex(const char *msg, char *hex)
{
    uint8_t d[32];
    axsha256((const uint8_t *)msg, strlen(msg), d);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
    {
        hex[i*2]   = hx[d[i] >> 4];
        hex[i*2+1] = hx[d[i] & 0xF];
    }
    hex[64] = 0;
}

#endif
