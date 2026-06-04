# Zcash v6.12.5 security backport — analysis & status

On 2026-06-01/02 the Zcash project shipped a **time-critical, coordinated
release** (`v6.12.5`, with a soft fork at upstream mainnet height 3363426) fixing
**seven** consensus / denial-of-service vulnerabilities in `zcashd`, all reachable
by a remote peer or miner. Zclassic 3.0 forked from `v6.12.3`, *before* these
fixes, so this document records what each fix is, whether it applies to Zclassic's
current consensus, and what we did about it.

Upstream reference: <https://github.com/zcash/zcash/releases/tag/v6.12.5>
(diff range `v6.12.3..v6.12.5`).

## TL;DR

| CVE (GHSA) | Class | Applies to Zclassic **today**? | Status |
| --- | --- | --- | --- |
| g4x5-crjh-29ff | Coinbase shielded value balance → `AbortNode` crash/crash-loop | **Yes** (Sapling is active) | **Ported** |
| 78pp-mc9g-g4mw | Out-of-range pool-value delta → unbanned peer → disk-write DoS | **Yes** (Sprout/Sapling pools active) | **Ported** |
| (DoS tuning) | Ban score for building atop an invalid block | Yes (hardening) | **Ported** |
| rpcw-q5mr-gq35 | NU5+ block-body poisoning via `hashAuthDataRoot` | No — NU5 not activated | Deferred (see below) |
| qvwc-hc2r-82qv | v5 `scriptSig` sigop mutation (authdata) | No — v5 tx rejected | Deferred |
| wmwc-773c-qcvv | `bad-cb-length` header poisoning (authdata) | No — NU5 not activated | Deferred |
| 382w-958v-m5jr | `bad-blk-length` header poisoning (authdata) | No — NU5 not activated | Deferred |
| ghc3-g8w4-whf9 | Orchard precaution (temporarily disable Orchard actions) | No — Orchard never activated | N/A while dormant |

**Why the split.** Zclassic caps transactions at Sapling (v4): NU5 is held at a
never-activating height, so **v5/ZIP-225 transactions and Orchard actions are
rejected by consensus today**. The four "block-body poisoning" CVEs all depend on
v5 *auth-data* (`scriptSig`, binding/spend-auth sigs, proofs) being committed by
`hashAuthDataRoot` *separately* from the txid Merkle root — a split that only
exists from NU5 onward. Pre-NU5, `hashMerkleRoot` pins the entire block body, so
those rejections are not body-replaceable and the vulnerability is not reachable
on Zclassic's current consensus. The two CVEs that touch the **Sapling/Sprout**
value pools and coinbase accounting *are* reachable today, so they are ported.

## Ported now (present risk on Zclassic's live consensus)

All changes are in `src/main.cpp` and were applied **manually** (adapted to
Zclassic's diverged tree), preserving upstream semantics.

### GHSA-g4x5-crjh-29ff — coinbase shielded value balance crash

A coinbase transaction with a **positive Sapling (or Orchard) value balance**
desynchronizes chain-supply accounting from the pool balances in `ConnectBlock`:
the supply delta drops a positive value balance (via `GetValueOut`) while the
pool delta subtracts it, so the supply-consistency check sees a mismatch and calls
`AbortNode` *before* the binding-signature check that would have rejected the
bundle. Because `AbortNode` leaves the block `MODE_ERROR` (not `MODE_INVALID`), it
is retried on restart — a remotely-triggerable **crash loop**.

Fix (ported):
1. `CheckTransaction` (`CheckTransactionWithoutProofVerification`) now rejects a
   positive coinbase Sapling value balance (`bad-cb-positive-sapling-valuebalance`)
   and a positive coinbase Orchard value balance
   (`bad-cb-positive-orchard-valuebalance`) — *before* the supply check is ever
   reached.
2. **Defence in depth:** the Sapling and Orchard **binding-signature
   validations** in `ConnectBlock` are moved to run *before* the supply-consistency
   check (immediately after the anchors are pushed, before the `if (!fJustCheck)`
   block), so an invalid bundle is cleanly rejected rather than aborting the node.

### GHSA-78pp-mc9g-g4mw — out-of-range pool-value delta DoS

If a block's aggregate per-pool value delta falls outside the valid monetary
range, `ReceivedBlockTransactions` previously returned a bare `error()` leaving
`state` `MODE_VALID`: no DoS score, so the peer was never banned, and the block
index stayed header-only — letting a peer **replay the same P2P block message to
re-write the block body to disk indefinitely**.

Fix (ported): route the `SetChainPoolValues` failure (and the same-peer
`AccumulateChainPoolValues` failure) through `state.DoS(100, …,
"bad-blk-pool-value-out-of-range")`. The overflow is a deterministic property of
the block body, so banning the sending peer is safe. Descendant blocks linked in
from `mapBlocksUnlinked` (possibly from other peers) are left for `ConnectBlock`
to reject, to avoid mis-attributing the ban.

### DoS tuning — building atop an invalid block

`AcceptBlockHeader` now applies a DoS score of **0** (was 100) when a peer relays
a header whose previous block is already known-invalid: a peer on a doomed fork is
not necessarily malicious. The header is still rejected. (Upstream pairs this with
the `BodyCorruption::HeaderOnly` classification from the deferred refactor below;
we apply only the score change, matching our current `CValidationState::DoS`
signature.)

## Deferred — required before activating NU5 / Orchard

These are **not exploitable on Zclassic's current (Sapling-capped) consensus** and
are intentionally **not** ported yet, because (a) they do not protect anything
reachable today, and (b) one of them is an invasive, file-wide change that should
land with a full build/test cycle, which this consensus node demands. They are
**prerequisites for any future network upgrade that activates v5/NU5 or Orchard**,
and must be pulled at that time.

### Block-body poisoning class (rpcw-q5mr-gq35, qvwc-hc2r-82qv, wmwc-773c-qcvv, 382w-958v-m5jr)

Upstream closes this structurally by tracking, on `CValidationState`, whether each
header-to-body commitment (`hashMerkleRoot`, and NU5+ `hashAuthDataRoot` via
`hashBlockCommitments`) has been verified against the body before a body-derived
rejection may be cached as permanent header invalidity. The mechanism replaces the
`bool corruptionIn` parameter of `CValidationState::DoS` with a `BodyCorruption`
enum (`Possible` / `Default` / `HeaderOnly`) and adds commitment-tracking flags
that are set in `CheckBlock` / `CheckBlockBodyAuthCommitment` and reset per
validation pass.

Upstream commits to port (in order):
- `85a2ffb81` Revert "Drop `corruptionIn=true` from `bad-blk-sigops` rejection."
- `24cf22a67` consensus: Run active-tip auth-commitment pre-check before CheckBlock
- `5d06d2a19` consensus: Track header-to-body commitments on CValidationState

This is a **signature change touching every `state.DoS(...)` call site** and is the
kind of change that must be compiled and run against the gtest suite
(`test_validation.cpp`, `test_checkblock.cpp`) — do it in a dedicated build
session, not blind.

### Orchard precaution (ghc3-g8w4-whf9)

Upstream temporarily **disables Orchard actions** by consensus rule
(`bad-tx-has-orchard-actions`, dropped from mempool and rejected from blocks) at a
soft fork, as a precaution while the underlying Orchard issue is remediated.

For Zclassic this is **already the case**: Orchard is never activated, so Orchard
actions are already rejected and Orchard bundles must be empty. The upstream soft
fork (and its mainnet heights 3363366 / 3363426) is therefore **not applicable**.

**Important for a future Orchard rollout:** do **not** re-enable Orchard by simply
scheduling activation heights. The upstream disable is a *stopgap*; activating
Orchard safely requires pulling the **actual Orchard remediation** that supersedes
it (track Zcash releases after v6.12.5 / the NU6.2 line, e.g. `orchard 0.14+` in
v6.20.0), plus the block-body-poisoning fixes above (which become live once v5/NU5
is active).

## Notes

- The large remainder of `v6.12.3..v6.12.5` (LLVM 22 / Rust 1.96 toolchain bumps,
  glibc back-compat removal, Boost 1.88, CI, dependency vendoring) is **not**
  security-relevant and is intentionally **not** pulled; it would be a large,
  risky change to Zclassic's pinned build for no consensus benefit.
- The ported changes are **review-verified, not compiled** in this environment
  (the consensus node requires the full `depends/` toolchain to build). Build and
  run the gtest suite before release.
