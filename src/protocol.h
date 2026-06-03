// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin Core developers
// Copyright (c) 2018-2023 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef __cplusplus
#error This header can only be compiled as C++.
#endif

#ifndef BITCOIN_PROTOCOL_H
#define BITCOIN_PROTOCOL_H

#include "netbase.h"
#include "serialize.h"
#include "uint256.h"
#include "version.h"

#include <stdint.h>
#include <string>

/** Message header.
 * (4) message start.
 * (12) command.
 * (4) size.
 * (4) checksum.
 */
class CMessageHeader
{
public:
    enum {
        MESSAGE_START_SIZE = 4,
        COMMAND_SIZE = 12,
        MESSAGE_SIZE_SIZE = 4,
        CHECKSUM_SIZE = 4,

        MESSAGE_SIZE_OFFSET = MESSAGE_START_SIZE + COMMAND_SIZE,
        CHECKSUM_OFFSET = MESSAGE_SIZE_OFFSET + MESSAGE_SIZE_SIZE,
        HEADER_SIZE = MESSAGE_START_SIZE + COMMAND_SIZE + MESSAGE_SIZE_SIZE + CHECKSUM_SIZE
    };
    typedef unsigned char MessageStartChars[MESSAGE_START_SIZE];

    CMessageHeader(const MessageStartChars& pchMessageStartIn);
    CMessageHeader(const MessageStartChars& pchMessageStartIn, const char* pszCommand, unsigned int nMessageSizeIn);

    std::string GetCommand() const;
    bool IsValid(const MessageStartChars& messageStart) const;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(FLATDATA(pchMessageStart));
        READWRITE(FLATDATA(pchCommand));
        READWRITE(nMessageSize);
        READWRITE(FLATDATA(pchChecksum));
    }

    char pchMessageStart[MESSAGE_START_SIZE];
    char pchCommand[COMMAND_SIZE];
    uint32_t nMessageSize;
    uint8_t pchChecksum[CHECKSUM_SIZE];
};

/** nServices flags */
enum {
    // NODE_NETWORK means that the node is capable of serving the block chain. It is currently
    // set by all Bitcoin Core nodes, and is unset by SPV clients or other peers that just want
    // network services but don't provide them.
    NODE_NETWORK = (1 << 0),
    // NODE_BLOOM means the node is capable and willing to handle bloom-filtered connections.
    // Zcash nodes used to support this by default, without advertising this bit,
    // but no longer do as of protocol version 170004 (= NO_BLOOM_VERSION)
    NODE_BLOOM = (1 << 2),

    // Bits 24-31 are reserved for temporary experiments. Just pick a bit that
    // isn't getting used, or one not being used much, and notify the
    // bitcoin-development mailing list. Remember that service bits are just
    // unauthenticated advertisements, so your code must be robust against
    // collisions and other cases where nodes may be advertising a service they
    // do not actually support. Other service bits should be allocated via the
    // BIP process.
};

/** A CService with information about it as peer */
class CAddress : public CService
{
public:
    CAddress();
    explicit CAddress(CService ipIn, uint64_t nServicesIn = NODE_NETWORK);

    void Init();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        if (ser_action.ForRead())
            Init();
        int nVersion = s.GetVersion();
        if (s.GetType() & SER_DISK)
            READWRITE(nVersion);
        if ((s.GetType() & SER_DISK) ||
            (nVersion >= CADDR_TIME_VERSION && !(s.GetType() & SER_GETHASH)))
            READWRITE(nTime);
        READWRITE(nServices);
        READWRITE(*(CService*)this);
    }

    // TODO: make private (improves encapsulation)
public:
    uint64_t nServices;

    // disk and network only
    unsigned int nTime;
};

//! Maximum number of addresses accepted in a single `addr` / `addrv2` message.
static const unsigned int MAX_ADDRV2_TO_RECV = 1000;

/**
 * Isolated BIP155 (`addrv2`) serializer for a vector<CAddress>.
 *
 * Writes only the addrv2 wire format and never touches the legacy V1
 * CAddress::SerializationOp (which is used by `addr` and peers.dat), so it
 * cannot perturb those byte-for-byte-stable encodings. Addresses whose network
 * is not representable in addrv2 (e.g. legacy Tor v2) are skipped.
 *
 * Per-address wire format (BIP155):
 *   uint32      nTime (LE)
 *   CompactSize nServices
 *   uint8       networkID
 *   CompactSize addrlen
 *   uint8[]     addr bytes
 *   uint16      port (big-endian)
 */
class CAddrV2Writer
{
    const std::vector<CAddress>& m_vec;
public:
    explicit CAddrV2Writer(const std::vector<CAddress>& vec) : m_vec(vec) {}

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        std::vector<const CAddress*> sendable;
        sendable.reserve(m_vec.size());
        for (const CAddress& a : m_vec) {
            if (a.GetBIP155Network() != 0)
                sendable.push_back(&a);
        }
        WriteCompactSize(s, sendable.size());
        for (const CAddress* pa : sendable) {
            const CAddress& addr = *pa;
            ser_writedata32(s, addr.nTime);
            WriteCompactSize(s, addr.nServices);
            ser_writedata8(s, addr.GetBIP155Network());
            std::vector<unsigned char> bytes = addr.GetAddrV2Bytes();
            WriteCompactSize(s, bytes.size());
            if (!bytes.empty())
                s.write((const char*)bytes.data(), bytes.size());
            unsigned short port = addr.GetPort();
            ser_writedata8(s, (unsigned char)((port >> 8) & 0xff));
            ser_writedata8(s, (unsigned char)(port & 0xff));
        }
    }
};

/** Parse an `addrv2` message body into vAddr, skipping unsupported networks. */
template <typename Stream>
void UnserializeAddrV2(Stream& s, std::vector<CAddress>& vAddr)
{
    uint64_t count = ReadCompactSize(s);
    if (count > MAX_ADDRV2_TO_RECV)
        throw std::ios_base::failure("addrv2 message size exceeds limit");
    vAddr.clear();
    vAddr.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t nTime = ser_readdata32(s);
        uint64_t nServices = ReadCompactSize(s);
        uint8_t network_id = ser_readdata8(s);
        uint64_t addrlen = ReadCompactSize(s);
        // Generous upper bound; representable networks are <= 32 bytes, but we
        // must still consume the bytes of unknown-but-well-formed networks.
        if (addrlen > 512)
            throw std::ios_base::failure("addrv2 address field too long");
        std::vector<unsigned char> bytes(addrlen);
        if (addrlen)
            s.read((char*)bytes.data(), addrlen);
        uint8_t port_hi = ser_readdata8(s);
        uint8_t port_lo = ser_readdata8(s);
        unsigned short port = ((uint16_t)port_hi << 8) | port_lo;

        CNetAddr na;
        if (na.SetBIP155(network_id, bytes)) {
            CAddress addr(CService(na, port), nServices);
            addr.nTime = nTime;
            vAddr.push_back(addr);
        }
        // Unknown / unsupported networks are skipped (bytes already consumed).
    }
}

/** getdata / inv message types.
 * These numbers are defined by the protocol. When adding a new value, be sure
 * to mention it in the respective ZIP, as well as checking for collisions with
 * BIPs we might want to backport.
 */
enum GetDataMsg : uint32_t {
    UNDEFINED = 0,
    MSG_TX = 1,
    MSG_BLOCK = 2,
    MSG_WTX = 5,             //!< Defined in ZIP 239
    // The following can only occur in getdata. Invs always use TX/WTX or BLOCK.
    MSG_FILTERED_BLOCK = 3,  //!< Defined in BIP37
};

/** inv message data */
class CInv
{
public:
    CInv();
    CInv(int typeIn, const uint256& hashIn);
    CInv(int typeIn, const uint256& hashIn, const uint256& hashAuxIn);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        int nVersion = s.GetVersion();
        READWRITE(type);

        // The implicit P2P network protocol inherited from Bitcoin Core has
        // zclassicd nodes sort-of ignoring unknown CInv message types in inv
        // messages: they are added to the known transaction inventory, but
        // AlreadyHave returns true, so we do nothing with them. Meanwhile for
        // getdata messages, ProcessGetData ignores unknown message types
        // entirely.
        //
        // As of v4.5.0, we change the implementation behaviour to reject
        // undefined message types instead of ignoring them.
        switch (type) {
        case MSG_TX:
        case MSG_BLOCK:
        case MSG_FILTERED_BLOCK:
            break;
        case MSG_WTX:
            if (nVersion < CINV_WTX_VERSION) {
                throw std::ios_base::failure(
                    "Negotiated protocol version does not support CInv message type MSG_WTX");
            }
            break;
        default:
            // This includes UNDEFINED, which should never be serialized.
            throw std::ios_base::failure("Unknown CInv message type");
        }

        READWRITE(hash);
        if (type == MSG_WTX) {
            // We've already checked above that nVersion >= CINV_WTX_VERSION.
            READWRITE(hashAux);
        } else if (type == MSG_TX && ser_action.ForRead()) {
            // Ensure that this value is set consistently in memory for MSG_TX.
            hashAux = LEGACY_TX_AUTH_DIGEST;
        }
    }

    friend bool operator<(const CInv& a, const CInv& b);

    std::string GetCommand() const;
    std::vector<unsigned char> GetWideHash() const;
    std::string ToString() const;

    // TODO: make private (improves encapsulation)
public:
    int type;
    // The main hash. This is:
    // - MSG_BLOCK: the block hash.
    // - MSG_TX and MSG_WTX: the txid.
    uint256 hash;
    // The auxiliary hash. This is:
    // - MSG_BLOCK: null (all-zeroes) and not parsed or serialized.
    // - MSG_TX: legacy auth digest (all-ones) and not parsed or serialized.
    // - MSG_WTX: the auth digest.
    uint256 hashAux;
};

#endif // BITCOIN_PROTOCOL_H
