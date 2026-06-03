// Copyright (c) 2020-2021 The Bitcoin Core developers
// Copyright (c) 2025 The Zclassic developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "crypto/sha3.h"

#include <cstring>

namespace {

inline uint64_t Rotl(uint64_t x, int n)
{
    return (x << n) | (x >> (64 - n));
}

// Keccak-f[1600] permutation (24 rounds).
void KeccakF(uint64_t (&st)[25])
{
    static const uint64_t RNDC[24] = {
        0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
        0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
        0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
        0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
        0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
        0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
        0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
        0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
    static const int ROTC[24] = {
        1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
        27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};
    static const int PILN[24] = {
        10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
        15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

    for (int round = 0; round < 24; ++round) {
        uint64_t bc[5];

        // Theta
        for (int i = 0; i < 5; ++i)
            bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
        for (int i = 0; i < 5; ++i) {
            uint64_t t = bc[(i + 4) % 5] ^ Rotl(bc[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5)
                st[j + i] ^= t;
        }

        // Rho + Pi
        uint64_t t = st[1];
        for (int i = 0; i < 24; ++i) {
            int j = PILN[i];
            bc[0] = st[j];
            st[j] = Rotl(t, ROTC[i]);
            t = bc[0];
        }

        // Chi
        for (int j = 0; j < 25; j += 5) {
            for (int i = 0; i < 5; ++i)
                bc[i] = st[j + i];
            for (int i = 0; i < 5; ++i)
                st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
        }

        // Iota
        st[0] ^= RNDC[round];
    }
}

inline void AbsorbBlock(uint64_t (&st)[25], const unsigned char* block)
{
    for (int i = 0; i < (int)(SHA3_256::RATE / 8); ++i) {
        uint64_t lane = 0;
        for (int b = 0; b < 8; ++b)
            lane |= (uint64_t)block[i * 8 + b] << (8 * b);
        st[i] ^= lane;
    }
    KeccakF(st);
}

} // namespace

SHA3_256& SHA3_256::Reset()
{
    memset(m_state, 0, sizeof(m_state));
    m_bufsize = 0;
    return *this;
}

SHA3_256& SHA3_256::Write(const unsigned char* data, size_t len)
{
    while (len > 0) {
        unsigned take = RATE - m_bufsize;
        if (take > len) take = (unsigned)len;
        memcpy(m_buffer + m_bufsize, data, take);
        m_bufsize += take;
        data += take;
        len -= take;
        if (m_bufsize == RATE) {
            AbsorbBlock(m_state, m_buffer);
            m_bufsize = 0;
        }
    }
    return *this;
}

SHA3_256& SHA3_256::Finalize(unsigned char hash[OUTPUT_SIZE])
{
    // pad10*1 with the SHA-3 domain-separation byte 0x06.
    m_buffer[m_bufsize] = 0x06;
    for (unsigned i = m_bufsize + 1; i < RATE; ++i)
        m_buffer[i] = 0;
    m_buffer[RATE - 1] |= 0x80;
    AbsorbBlock(m_state, m_buffer);

    // Squeeze the first 32 bytes (4 little-endian lanes).
    for (int i = 0; i < (int)(OUTPUT_SIZE / 8); ++i)
        for (int b = 0; b < 8; ++b)
            hash[i * 8 + b] = (unsigned char)((m_state[i] >> (8 * b)) & 0xff);

    Reset();
    return *this;
}

bool SHA3_256_SelfTest()
{
    // FIPS 202 SHA3-256("") =
    //   a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a
    static const unsigned char expected[SHA3_256::OUTPUT_SIZE] = {
        0xa7, 0xff, 0xc6, 0xf8, 0xbf, 0x1e, 0xd7, 0x66,
        0x51, 0xc1, 0x47, 0x56, 0xa0, 0x61, 0xd6, 0x62,
        0xf5, 0x80, 0xff, 0x4d, 0xe4, 0x3b, 0x49, 0xfa,
        0x82, 0xd8, 0x0a, 0x4b, 0x80, 0xf8, 0x43, 0x4a};
    unsigned char out[SHA3_256::OUTPUT_SIZE];
    SHA3_256().Finalize(out);
    return memcmp(out, expected, sizeof(out)) == 0;
}
