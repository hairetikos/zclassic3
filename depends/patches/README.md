# Zclassic Rust crate patches

Zclassic uses consensus branch IDs that are not part of upstream Zcash, but the
modern Rust crypto stack (`zcash_protocol` / `zcash_primitives`) hardcodes
Zcash's branch IDs in the `BranchId` enum. This directory contains patches that
adapt those crates for Zclassic.

## `apply-zcash-protocol-branchid-patch.sh`

Repurposes the **Canopy** consensus branch ID (`0xe9ff75a6`, which Zclassic never
uses because it does not activate Canopy) to Zclassic's **Buttercup** branch ID
**`0x930b540d`**, in a locally-vendored copy of `zcash_protocol` under
`depends/patched/zcash_protocol`.

Why this is necessary: zcashd builds and signs transactions in Rust. The v4
(Sapling) sighash embeds the consensus branch ID as a literal value, so a
Zclassic transaction at the current epoch must be signed with `0x930b540d`.
`BranchId::try_from(0x930b540d)` previously returned `Err("Unknown consensus
branch ID")`, aborting the node when sending a shielded transaction. After the
patch, `0x930b540d` round-trips through `BranchId::try_from` / `From<BranchId>`
as the (renumbered) `Canopy` variant, which is a pre-NU5, v4-era branch — so
transactions parse and sign correctly and reuse the crate's tested ZIP-243
sighash implementation.

This does **not** affect ZIP-212 enforcement, which keys off the network's Canopy
*activation height* (disabled for Zclassic in `src/rust/src/params.rs`), not this
branch-ID constant.

The script is idempotent and is run automatically by `zcutil/build.sh` (including
`-rebuild`). The generated `depends/patched/` directory is git-ignored. To
re-vendor after a `zcash_protocol` version bump, delete `depends/patched/` and
rebuild.

The `[patch.crates-io]` entry that activates the vendored crate lives in the
workspace `Cargo.toml`.
