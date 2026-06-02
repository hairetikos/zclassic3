#!/bin/sh
# Zclassic: patch the zcash_protocol crate's consensus branch IDs.
#
# Why: zcashd builds transactions in Rust (librustzcash), which converts the
# consensus branch ID into the upstream `zcash_protocol::consensus::BranchId`
# enum. That enum only knows Zcash's branch IDs, so it rejects Zclassic's
# Buttercup branch ID 0x930b540d with "Unknown consensus branch ID" and the node
# aborts when signing a shielded transaction. The v4 (Sapling) sighash embeds the
# branch ID as a literal value, so we cannot substitute a Zcash branch ID without
# producing signatures the Zclassic network rejects.
#
# Fix: Zclassic never activates Canopy, so its Canopy branch-ID constant
# (0xe9ff75a6) is unused. We repurpose it to Zclassic's Buttercup branch ID
# 0x930b540d. Then BranchId::try_from(0x930b540d) -> Canopy, u32::from(Canopy) ->
# 0x930b540d (correct sighash), and Transaction::read parses as v4 (Canopy is a
# pre-NU5, v4-era branch). This reuses the crate's tested ZIP-243 sighash code
# with the correct branch-ID value. ZIP-212 enforcement is unaffected: it keys
# off the network's Canopy *activation height* (disabled for Zclassic, set in
# src/rust/src/params.rs), not this branch-ID constant.
#
# The patched crate is vendored under depends/patched/ and wired in via
# [patch.crates-io] in the workspace Cargo.toml. This script is idempotent and is
# run automatically by zcutil/build.sh.

set -eu

ZCL_BUTTERCUP_BRANCH_ID_UNDERSCORE="0x930b_540d"
CANOPY_BRANCH_ID_UNDERSCORE="0xe9ff_75a6"
CANOPY_BRANCH_ID_PLAIN="0xe9ff75a6"

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
DEST="$REPO_ROOT/depends/patched/zcash_protocol"
CARGO_HOME_DIR="${CARGO_HOME:-$HOME/.cargo}"

# Resolve the zcash_protocol version cargo is using (prefer Cargo.lock).
VER=$(awk '
    /^name = "zcash_protocol"$/ { f=1; next }
    f && /^version = / { gsub(/[",]/, "", $3); print $3; exit }
' "$REPO_ROOT/Cargo.lock" 2>/dev/null || true)

MARKER="$DEST/.zclassic-branchid-patched"
if [ -f "$MARKER" ] && { [ -z "$VER" ] || [ "$(cat "$MARKER" 2>/dev/null)" = "$VER" ]; }; then
    # Already vendored and patched for this version.
    exit 0
fi

find_src() {
    # Exact version match first, then highest 0.7.* as a fallback.
    if [ -n "$VER" ]; then
        for d in "$CARGO_HOME_DIR"/registry/src/*/"zcash_protocol-$VER"; do
            [ -d "$d" ] && { echo "$d"; return; }
        done
    fi
    ls -d "$CARGO_HOME_DIR"/registry/src/*/zcash_protocol-0.7.* 2>/dev/null | sort -V | tail -1
}

SRC=$(find_src || true)

if [ -z "${SRC:-}" ] || [ ! -d "${SRC:-/nonexistent}" ]; then
    # Not in the cache yet (e.g. a completely fresh checkout). Populate it via a
    # throwaway project so we don't depend on the main Cargo.toml (which already
    # references this not-yet-created patch path).
    echo "zcash_protocol source not found in cargo cache; fetching it..."
    TMP=$(mktemp -d)
    (
        cd "$TMP"
        cargo new --quiet --lib zclfetch >/dev/null 2>&1 || true
        cd zclfetch
        printf 'zcash_protocol = "0.7"\n' >> Cargo.toml
        cargo fetch --quiet >/dev/null 2>&1 || true
    ) || true
    rm -rf "$TMP"
    SRC=$(find_src || true)
fi

if [ -z "${SRC:-}" ] || [ ! -d "${SRC:-/nonexistent}" ]; then
    echo "ERROR: could not locate the zcash_protocol crate source." >&2
    echo "Run 'cargo fetch' once (with the [patch] line temporarily removed from" >&2
    echo "Cargo.toml if needed) to populate ~/.cargo, then re-run this script." >&2
    exit 1
fi

echo "Vendoring zcash_protocol from: $SRC"
rm -rf "$DEST"
mkdir -p "$(dirname "$DEST")"
cp -a "$SRC" "$DEST"
# Registry sources are read-only; make the copy writable so we can patch it.
chmod -R u+w "$DEST"
# Registry checksum file is meaningless for a path patch and we are modifying the
# source, so drop it.
rm -f "$DEST/.cargo-checksum.json"

CONSENSUS="$DEST/src/consensus.rs"
if [ ! -f "$CONSENSUS" ]; then
    echo "ERROR: expected $CONSENSUS to exist in the vendored crate." >&2
    exit 1
fi

# Repurpose the Canopy branch-ID constant -> Zclassic Buttercup 0x930b540d in
# both directions (TryFrom<u32> and From<BranchId>), and any in-file test
# literals, so the crate stays internally consistent.
sed -i.bak \
    -e "s/$CANOPY_BRANCH_ID_UNDERSCORE/$ZCL_BUTTERCUP_BRANCH_ID_UNDERSCORE/g" \
    -e "s/$CANOPY_BRANCH_ID_PLAIN/0x930b540d/g" \
    "$CONSENSUS"
rm -f "$CONSENSUS.bak"

if ! grep -q "$ZCL_BUTTERCUP_BRANCH_ID_UNDERSCORE" "$CONSENSUS"; then
    echo "ERROR: branch-ID patch did not apply to $CONSENSUS" >&2
    exit 1
fi
if grep -q "$CANOPY_BRANCH_ID_UNDERSCORE" "$CONSENSUS"; then
    echo "ERROR: residual Canopy branch ID remains in $CONSENSUS after patching" >&2
    exit 1
fi

printf '%s' "${VER:-unknown}" > "$MARKER"
echo "Patched zcash_protocol: Canopy branch ID -> 0x930b540d (Zclassic Buttercup)."
