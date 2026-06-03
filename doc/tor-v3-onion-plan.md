# Tor Onion v3 networking for Zclassic — implementation plan

## Status / TL;DR

The Zcash codebase Zclassic is forked from still uses the **pre-BIP155** Bitcoin
networking stack:

- `CNetAddr` stores a fixed `unsigned char ip[16]` (`src/netbase.h`).
- Tor support is **v2 only**, via the legacy OnionCat encoding
  (`pchOnionCat` `{0xFD,0x87,0xD8,0x7E,0xEB,0x43}` + 10 decoded bytes → 16 bytes;
  `EncodeBase32(&ip[6], 10) + ".onion"`).
- `torcontrol.cpp` explicitly requests `NEW:RSA1024` (Tor v2 keys).
- There is no `addrv2` / `sendaddrv2` P2P message.

Tor **v2 onion services were deprecated and disabled** by the Tor network in
2021. A v3 onion address is `base32(ed25519_pubkey[32] || checksum[2] || 0x03)`
= 56 characters; the underlying identifier is the **32-byte ed25519 public key**,
which does **not** fit in the legacy 16-byte `ip`. So v3 support requires changing
the fundamental address representation.

This is exactly the work Bitcoin Core did for v0.21.0:

- **BIP155 `addrv2`** + variable-length `CNetAddr` rework — Bitcoin Core PR
  [#19031](https://github.com/bitcoin/bitcoin/pull/19031) (and the netaddress
  split #17192).
- **Tor v3 hidden services** (`ED25519-V3` in the control protocol) — PR
  [#19954](https://github.com/bitcoin/bitcoin/pull/19954).
- Follow-ups: #20685 (gather/relay), #21564, etc.

**Reference baseline: Bitcoin Core v0.21.0** (first release with full Tor v3 +
BIP155). All of this is long-merged and stable upstream, so we port from it
rather than inventing anything.

---

## Design

### 1. Address representation (`src/netbase.h`, `src/netbase.cpp`)

Replace `unsigned char ip[16]` with Bitcoin's variable-length representation:

```cpp
enum Network {
    NET_UNROUTABLE = 0,
    NET_IPV4,
    NET_IPV6,
    NET_ONION,        // was NET_TOR; now means Tor v3 (v2 is dropped/Unroutable)
    NET_I2P,          // reserve the slot for parity, even if unused for now
    NET_CJDNS,        // reserve the slot
    NET_INTERNAL,
    NET_MAX,
};

// BIP155 network ids (wire encoding for addrv2)
enum BIP155Network : uint8_t {
    IPV4 = 1, IPV6 = 2, TORV2 = 3, TORV3 = 4, I2P = 5, CJDNS = 6,
};

// sizes
static constexpr size_t ADDR_IPV4_SIZE  = 4;
static constexpr size_t ADDR_IPV6_SIZE  = 16;
static constexpr size_t ADDR_TORV2_SIZE = 10;
static constexpr size_t ADDR_TORV3_SIZE = 32;

class CNetAddr {
    prevector<ADDR_IPV6_SIZE, uint8_t> m_addr{};  // raw address bytes
    Network m_net{NET_IPV4};
    uint32_t m_scope_id{0};                        // for IPv6 link-local
    ...
};
```

Port all the `ip[...]`-indexing methods to operate on `m_addr` / `m_net`
(`IsIPv4`, `IsIPv6`, the `IsRFCxxxx`, `IsTor`→`IsTorV3`, `IsLocal`, `IsRoutable`,
`IsValid`, `GetNetwork`, `ToStringIP`, `GetByte`, `GetHash`, `GetInAddr`,
`GetIn6Addr`, `GetGroup`, `GetReachabilityFrom`, `operator==/<`), plus
`CService::GetSockAddr/SetSockAddr/GetKey/ToStringIPPort` and `CSubNet::Match`.
This is a faithful port of `src/netaddress.cpp` from Bitcoin Core v0.21.

`SetSpecial(host)` learns to parse **both**:
- v2 (16 base32 chars → 10 bytes) — accept for back-compat parsing, but treat as
  `NET_UNROUTABLE` (the Tor network no longer routes v2), and
- **v3** (56 base32 chars → decode, verify the version byte `0x03` and the
  2-byte SHA3-256 checksum, store the 32-byte pubkey as `NET_ONION`).

### 2. Serialization (the careful part)

Two formats, exactly as Bitcoin:

- **V1 (legacy)** — used by the current `addr` message and by `peers.dat` until
  it is upgraded. Always 16 bytes, produced by mapping `m_net`/`m_addr` to the
  old encoding (IPv4 → `::FFFF:0:0/96`-mapped, IPv6 → raw, TORv2 → OnionCat).
  **v3 (and any address not representable in 16 bytes) is serialized as
  all-zero / treated as unroutable** and is **never advertised over the legacy
  `addr` message** — so we cannot corrupt the wire format or `peers.dat`.
  Byte-for-byte identical to today for IPv4/IPv6/TORv2.
- **V2 (BIP155 / `addrv2`)** — `network_id (1 byte) || CompactSize(len) || bytes`.
  Used by the new `addrv2` message and by the upgraded `peers.dat`.

Implement via stream-parameter–selected serialization (mirroring Bitcoin's
`SerializeV1Stream`/`SerializeV2Stream` / the `ADDRV2_FORMAT` flag) so the same
`CNetAddr`/`CService`/`CAddress` types serialize either way depending on context.

### 3. `addrv2` / `sendaddrv2` P2P messages (`src/protocol.{h,cpp}`, `src/main.cpp`)

- Add `NetMsgType::ADDRV2` (`"addrv2"`) and `NetMsgType::SENDADDRV2`
  (`"sendaddrv2"`).
- On `VERACK`/version handshake, send `sendaddrv2` to advertise support (BIP155
  ordering: before `verack`).
- Track `m_wants_addrv2` per peer; relay addresses to a peer using `addrv2` if it
  supports it, else fall back to legacy `addr` (omitting non-representable
  addresses such as v3).
- Parse incoming `addrv2` with the V2 stream format; gate count/size limits the
  same way `addr` is gated.

### 4. Tor control: v3 hidden services (`src/torcontrol.cpp`)

- Request `ADD_ONION NEW:ED25519-V3 ...` instead of `NEW:RSA1024`.
- Persist the returned `ED25519-V3:<key>` to the onion key file and re-use it via
  the existing `ADD_ONION <saved-key>` path so the .onion address is stable
  across restarts (migrate the file name, e.g. `onion_v3_private_key`, leaving the
  old `onion_private_key` untouched).
- Parse the 56-char `ServiceID` into a `NET_ONION` `CService` (now possible after
  the representation rework) and register it as a local address.

### 5. `CAddrMan` / `peers.dat` (`src/addrman.{h,cpp}`)

- Bump the `peers.dat` version and serialize with the V2 format so v3 addresses
  persist. Keep reading the old version (V1) for seamless upgrade; never write the
  old version once v3 is enabled.

### 6. SOCKS5 outbound to v3 (`src/netbase.cpp` `Socks5`)

Outbound `.onion` connections already go to Tor via SOCKS5 by **hostname**
(`ConnectSocketByName` → `Socks5` with the domain). v3 hostnames are 56 chars and
fit the SOCKS5 domain field (max 255), so connecting out to v3 works once the
address parses and `ToStringIP()` yields the correct 56-char `.onion`.

---

## Phased rollout (how this lands as PRs)

**Phase 1 — DONE: run a v3 service + connect out to v3.**
- `torcontrol.cpp` `ED25519-V3` + stable key persistence (`onion_v3_private_key`).
- v3 address representation in `CNetAddr`/`CService`, implemented as a **hybrid**
  rather than the full `m_addr` unification: `ip[16]` and all legacy IPv4/IPv6/v2
  code paths are left **byte-for-byte untouched** (zero risk to existing
  networking/serialization), and v3 is carried in an additive
  `m_addr_onion` blob (the 35-byte decoded onion: pubkey‖checksum‖version). This
  was chosen over the clean `m_addr` rework specifically because it could be
  landed without rebuilding/retesting the serialization of every existing address
  type. The full `m_addr` unification (uniform representation, IPv4 as 4 bytes,
  etc.) remains the eventual cleanup and is a prerequisite tidy-up for nothing
  functional — Phase 2 builds fine on the hybrid.
- v3 `.onion` parsing in `SetSpecial` (length + version-byte check; **checksum
  validation deferred** — it needs SHA3-256, which the tree does not yet bundle;
  Tor validates the checksum on connect). v2 stays parseable (deprecated).
- Outbound v3 works: `Lookup` → `ConnectSocket` → Tor SOCKS5 proxy using the
  56-char `.onion` from `ToStringIP()`.
- v3 addresses are **not** `AddLocal`'d, put into `addrman`, or gossiped: the
  legacy `addr`/`peers.dat` (V1) paths are untouched and never emit/clobber a v3
  address (a v3's `ip` is all-zero and `m_addr_onion` is not V1-serialized).
- Net effect: the node hosts a v3 hidden service (inbound) and can reach v3 peers
  given via `-addnode`/`-connect`. Self-advertisement/discovery is Phase 2.

**Phase 2 — in progress.**
- DONE: SHA3-256 (`src/crypto/sha3.{h,cpp}`, with a FIPS-202 self-test) and Tor v3
  onion **checksum validation** in `SetSpecial` (fails open only if the SHA3
  self-test fails, so it can never regress v3 parsing on a miscompile).
- **Still pending for "full" v3:** addrv2/`sendaddrv2` gossip, `addrman`/`peers.dat`
  V2 persistence, and (optionally) the clean `m_addr` unification + exact onion
  `CSubNet` matching.

**Phase 2: P2P discovery.**
- `addrv2`/`sendaddrv2`, per-peer negotiation and relay, V2 serialization.
- Store v3 in `addrman`; bump `peers.dat` to the V2 format (with V1 read-compat).
- Net effect: automatic v3 peer discovery/propagation.

**Phase 3: hardening & tests.**
- Port Bitcoin's `net_tests`/`netbase_tests` BIP155 vectors; regtest/testnet
  interop; fuzz the deserializer; `-onlynet=onion`, `-bind=...=onion` review.

---

## Risks & mitigations

- **Serialization is consensus-of-the-network-critical.** A V1 byte mismatch
  corrupts `peers.dat` or breaks `addr` gossip / forks us off address
  propagation. Mitigation: keep V1 byte-identical for IPv4/IPv6/TORv2; never
  V1-serialize v3; port Bitcoin's exact code and its test vectors; validate on
  testnet before mainnet.
- **`CNetAddr` touches a huge surface** (net, addrman, rpc/net, scripts). Compile
  breakage is likely until every `ip[]`-indexing site is migrated. Mitigation:
  do the rework as one atomic change and lean on the compiler.
- **Untested in this environment.** This branch must be built and exercised on
  regtest + testnet (create a v3 service, connect two nodes over Tor v3) before
  merge. The PR should not be merged on read-review alone.
- **v2 onion is dead.** We deliberately downgrade v2 to "parse but unroutable"
  rather than maintaining it.

## Testing strategy

- Unit: port `netbase_tests`/`net_tests` v3 parse + BIP155 (de)serialization
  vectors from Bitcoin v0.21.
- Regtest: two nodes, `-listenonion`, confirm `getnetworkinfo`/`getnodeaddresses`
  show a v3 local address; `addnode` a v3 peer.
- Testnet: real Tor, confirm inbound to our v3 service and outbound to a known v3
  peer; confirm legacy nodes still gossip with us over `addr`.
- Negative: malformed/short/long `.onion`, bad checksum, bad version byte.

## Future thoughts / "even better"

- **Drop v2 entirely** (reject parsing), since the network no longer routes it.
- **I2P (SAM v3)** and **CJDNS** support — BIP155 already reserves the network
  ids; Bitcoin added these in v22/v23. The representation rework here makes them
  incremental.
- **BIP324 v2 encrypted P2P transport** — orthogonal to Tor but a natural next
  privacy/robustness upgrade.
- **`-onlynet=onion` / `-proxy` ergonomics**, Tor stream isolation per peer
  (randomized SOCKS credentials — partly present via `randomize_credentials`).
- **Automatic `-listenonion` default** once v3 is stable, and advertising only
  the onion address when running `-onlynet=onion` (avoid leaking clearnet IP).
- **Address-relay privacy**: adopt Bitcoin's later addr-relay rate-limiting and
  `addr` privacy fixes while we are in this code.
- **Replace the deprecated `FLATDATA`/`ADD_SERIALIZE_METHODS`** serialization
  idioms with the newer ones if/when the serialization layer is modernized.
