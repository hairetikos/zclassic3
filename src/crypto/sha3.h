// Copyright (c) 2020-2021 The Bitcoin Core developers
// Copyright (c) 2025 The Zclassic developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_SHA3_H
#define BITCOIN_CRYPTO_SHA3_H

#include <cstdlib>
#include <stdint.h>

//! SHA3-256 (FIPS 202 / Keccak with the SHA-3 domain separation byte 0x06).
//!
//! Streaming one-shot-friendly implementation. Used by Zclassic for Tor v3
//! onion-address checksums (see torv3 helpers in netbase). Ported from Bitcoin
//! Core's crypto/sha3, adapted to a pointer/length API (no Span dependency).
class SHA3_256
{
private:
    uint64_t m_state[25];
    unsigned char m_buffer[136]; //!< rate of SHA3-256 in bytes: (1600 - 2*256)/8
    unsigned m_bufsize{0};

public:
    static constexpr size_t OUTPUT_SIZE = 32;
    static constexpr unsigned RATE = 136;

    SHA3_256() { Reset(); }

    SHA3_256& Write(const unsigned char* data, size_t len);
    SHA3_256& Finalize(unsigned char hash[OUTPUT_SIZE]);
    SHA3_256& Reset();
};

//! Known-answer self test: returns true iff SHA3-256("") matches the FIPS-202
//! test vector. Used to fail safe if the implementation is somehow miscompiled.
bool SHA3_256_SelfTest();

#endif // BITCOIN_CRYPTO_SHA3_H
