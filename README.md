Zclassic 3.0 — "sigma"
======================

**Zclassic (ZCL)** is a community-driven, fair-launch, privacy-preserving
cryptocurrency. There was no premine, no founders' reward, and no developer tax —
all coins are mined by the community. Zclassic uses the shielded-transaction
technology of the Zcash/Zerocash lineage (zk-SNARKs) to let users transact with
strong privacy.

This repository is **Zclassic 3.0**, a major modernization of the Zclassic full
node (`zclassicd`).

> ### 🧅 New in 3.0: Tor **Onion v3** support
>
> **This is the headline feature, and it matters for your privacy.** Tor
> **Onion v2 addresses were deprecated and turned off by the Tor network in
> 2021** — they no longer route at all. Upstream **Zcash never shipped Onion v3**
> and left its node software stuck on the dead v2 scheme, so running a Zcash-style
> node "over Tor" has effectively been broken for years.
>
> Zclassic 3.0 adds **initial native Tor Onion v3 support**: it creates a modern
> v3 (ed25519, 56-character `.onion`) hidden service for inbound connections,
> connects out to v3 peers, and gossips v3 addresses to other v3-capable nodes
> via the BIP155 `addrv2` protocol. **If privacy is why you are here, this is the
> release you want.**
>
> See [`doc/tor-v3-onion-plan.md`](doc/tor-v3-onion-plan.md) for the full design
> and current status.

What's new in 3.0
-----------------

- **Rebased onto modern `zcashd` (Zcash 6.12.3).** Previous Zclassic nodes ran on
  a years-old Zcash codebase. Zclassic 3.0 takes the **latest** upstream `zcashd`
  — with all of its accumulated performance, networking, wallet, and crypto
  improvements — and re-applies Zclassic's consensus on top of it, producing a
  drop-in node for the **live Zclassic mainnet**.
- **Initial Tor Onion v3** networking + hidden services (see above).
- **Faithful Zclassic consensus.** The full Zclassic upgrade history is ported and
  verified by a genesis→tip mainnet sync: Overwinter/Sapling (476969), **Bubbles**
  (585318, Equihash 200,9→192,7), **DiffAdj** (585322), and **Buttercup** (707000:
  150s→75s spacing, 12.5→6.25 ZCL subsidy, 840k→1.68M halving interval, +3
  triple-halving). Branch IDs, block reward, difficulty, and block/transaction
  size rules all match the existing network. See
  [`doc/zclassicd-port-plan.md`](doc/zclassicd-port-plan.md).
- **Sync-from-genesis with full verification.** Historical block/transaction
  size limits are enforced by height (generous up to the Buttercup upgrade, then
  the strict standard limits), so a brand-new node validates the whole chain
  from block 0 **without skipping transaction verification** — keeping the
  shielded-value anti-counterfeiting guarantees intact.
- **Zclassic branding** throughout: the binaries are `zclassicd`, `zclassic-cli`,
  and `zclassic-tx`.

The `zclassicd` Full Node
-------------------------

This repository hosts `zclassicd`, a Zclassic consensus node. It downloads and
stores the entire history of Zclassic transactions and validates them. Depending
on your hardware and network connection, the initial synchronization can take a
while.

`zclassicd` is derived (via Zcash) from a source fork of
[Bitcoin Core](https://github.com/bitcoin/bitcoin); the codebases have diverged
substantially.

Network facts (mainnet)
-----------------------

| Item | Value |
| --- | --- |
| Currency unit | `ZCL` |
| Default P2P port | `8033` |
| Default RPC port | `8232` |
| Transparent address prefix | `t1…` / `t3…` |
| Sapling shielded address prefix | `zs…` |
| BIP44 coin type | `147` |

#### :lock: Security Warnings

**Zclassic 3.0 is experimental and a work in progress.** Use it at your own risk.
Tor Onion v3 support is *initial* — exercise it on testnet and verify behavior
before relying on it for strong anonymity. Always keep encrypted backups of your
wallet.

Roadmap &amp; future direction
--------------------------

Zclassic 3.0 is a foundation, not a finish line. A few things are deliberately
in place for what comes next.

### Dormant modern features (kept in the code, not activated)

The rebase onto modern `zcashd` means this codebase **still contains** the newer
Zcash shielded machinery — **Orchard**, **unified addresses**, and **v5/ZIP-225
transactions** (NU5/NU6 logic) — but Zclassic does **not** activate any of it.
These features are held at "never activate" upgrade heights, so they are present
and forward-compatible but completely inert: the live Zclassic consensus is
unchanged.

This is a deliberate design choice (gate by activation height, never by
deletion). It means the Zclassic **community can decide, in the future, to turn
these on as a coordinated chain upgrade (hardfork)**. Crucially:

- **No one has to buy ZCL again.** Such an upgrade is an evolution of the *same*
  chain — your existing coins and balances carry straight over to the upgraded
  network. There is no new coin, no swap, and no migration purchase.
- Because the machinery is already compiled and tested, activating it would be
  primarily a matter of scheduling activation heights and minting
  Zclassic-specific consensus branch IDs — not a from-scratch rewrite.

(See [`doc/zclassicd-port-plan.md`](doc/zclassicd-port-plan.md) for exactly how
these features are kept dormant-but-enableable.)

### Planned: multi-cipher (cascade) wallet encryption

We plan to strengthen at-rest wallet protection well beyond a single cipher,
using layered **cipher cascades** so that breaking the wallet would require
breaking *every* layer:

- **Windows / macOS** — a **3-cipher cascade** of **AES → Serpent → Twofish**,
  in the style of VeraCrypt volumes.
- **Linux** — a **5-cipher cascade** that additionally layers in **Camellia**
  and a **final AES** pass (AES → Serpent → Twofish → Camellia → AES), built on
  `dm-crypt`.

The goal is defense-in-depth: independent, well-studied ciphers stacked so that a
weakness (or future break) in any single algorithm does not expose the wallet.

### Planned: privacy &amp; security hardening beyond Zcash

Zclassic intends to push **further than Zcash** on privacy and resilience,
including **quantum-resistant** (post-quantum) privacy and security features. As
practical post-quantum schemes mature, the aim is to harden Zclassic's shielded
transactions, key material, and network/identity layers against both present-day
and future (quantum-capable) adversaries.

These are stated intentions and active areas of work, not shipped features in
3.0; they are listed here so the community knows the direction of travel.

Getting Started
---------------

### Building

Build `zclassicd` along with most of its dependencies from source:

```
./zcutil/build.sh -j$(nproc)
```

Useful build options:

```
./zcutil/build.sh -rebuild -j$(nproc)        # fast incremental rebuild after edits
MARCH_NATIVE=1 ./zcutil/build.sh -j$(nproc)  # CPU-tuned binary (recommended)
OPTIMIZE=1     ./zcutil/build.sh -j$(nproc)  # -O3 -march=native
```

Zclassic is officially supported on Debian and Ubuntu.

### Running

```
./src/zclassicd                      # start the node
./src/zclassic-cli getinfo           # query a running node
./src/zclassic-cli help              # list RPC commands
```

To run as a Tor v3 hidden service, run a local Tor daemon with its control port
enabled and start `zclassicd` with `-listenonion=1` (the default proxy/control
settings match a standard Tor install). The node will create a persistent v3
`.onion` service and advertise it to v3-capable peers.

License
-------

For license information see the file [COPYING](COPYING).

Zclassic builds on the work of the Zcash and Bitcoin Core developers; see
[`doc/authors.md`](doc/authors.md).
