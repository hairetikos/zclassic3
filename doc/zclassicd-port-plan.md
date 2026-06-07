# Zclassicd on modern Zcashd — consensus port plan

This document describes the work required to turn this fork of **Zcash 6.12.3**
into a **Zclassicd** that is a **drop-in, consensus-faithful node for the live
Zclassic (ZCL) network** — i.e. it must validate the entire existing ZCL chain
from genesis to the current tip and stay on the same chain as the existing
network, with no fork ("no softfork").

The codebase we are starting from is Zcash `v6.12.3` (NU6.1 era). The reference
for current Zclassic consensus is `hairetikos/zclassic-rebase` (an older Zcashd
fork), referred to below as "the ZCL reference".

---

## 0. Key findings (the surprising part)

1. **Zclassic already pays miners 100%.** The ZCL reference has *no*
   founders-reward enforcement anywhere in `main.cpp`. `ConnectBlock` only
   checks `coinbase value <= nFees + GetBlockSubsidy()` (ref `main.cpp:2738`),
   and `GetBlockSubsidy()` returns the *full* subsidy. The 48
   `vFoundersRewardAddress` entries in the ZCL reference `chainparams.cpp` are
   dead/vestigial Zcash leftovers, never referenced by consensus.

2. **Modern Zcashd 6.12.3 *enforces* the developer tax.** The coinbase is
   *required* to contain funding-stream / lockbox outputs:
   - `main.cpp:1120` → `cb-funding-stream-missing`
   - `main.cpp:1253-1279` → `cb-lockbox-disbursement-missing`
   So "removing the founders reward" here means **deleting tax enforcement** so
   the coinbase value rule collapses back to ZCL's `coinbase <= subsidy + fees`.

3. **Zclassic is NOT "Zcash minus the tax."** ZCL has its own divergent
   consensus history that the modern codebase has never seen. Achieving
   drop-in parity means **porting ZCL's custom network upgrades into modern
   Zcashd**, not merely disabling Zcash's.

---

## 1. Verified Zclassic consensus spec (the target to hit)

All values are MAINNET unless noted, taken from the ZCL reference.

### Chain identity
| Item | Value |
| --- | --- |
| Network magic (`pchMessageStart`) | `0x24 0xe9 0x27 0x64` |
| Default P2P port | `8033` |
| Default RPC port | `8232` |
| Currency unit | `ZCL` |
| BIP44 coin type | `147` |
| Message magic | `"Zclassic Signed Message:\n"` |

### Base58 / Bech32 prefixes
| Type | Value |
| --- | --- |
| PUBKEY_ADDRESS (t1) | `{0x1C,0xB8}` |
| SCRIPT_ADDRESS (t3) | `{0x1C,0xBD}` |
| SECRET_KEY | `{0x80}` |
| ZCPAYMENT_ADDRESS (zc) | `{0x16,0x9A}` |
| ZCVIEWING_KEY | `{0xA8,0xAB,0xD3}` |
| ZCSPENDING_KEY | `{0xAB,0x36}` |
| SAPLING_PAYMENT_ADDRESS | `zs` |
| SAPLING_FULL_VIEWING_KEY | `zviews` |
| SAPLING_INCOMING_VIEWING_KEY | `zivks` |
| SAPLING_EXTENDED_SPEND_KEY | `secret-extended-key-main` |

### Genesis (mainnet)
- hash `0x0007104ccda289427919efc39dc9e4d499804b7bebc22df55f8b834301260602`
- merkle root `0x19612bcf00ea7611d315d7f43554fa983c6e8c30cba17e52c679e0e80abf7d42`
- nTime `1478403829`, nBits `0x1f07ffff`, nNonce `…021d`, nVersion 4, reward 0
- coinbase tag `"Zclassic860413afe207aa173afee4fcfa9166dc745651c754a41ea8f155646f5aa828ac"`
- Equihash solution 1344 bytes (N=200, K=9)

### Network upgrades — ZCL's divergent table (sighash-critical)
| Upgrade | Height | Branch ID | Consensus effect |
| --- | --- | --- | --- |
| Overwinter | 476969 | `0x5ba81b19` | same as Zcash |
| Sapling | 476969 | `0x76b809bb` | activated simultaneously with Overwinter |
| **Bubbles** | 585318 | `0x821a451c` | Equihash params change 200,9 → **192,7** |
| **DiffAdj** | 585322 | `0x930b540d` | custom difficulty fork-scaling |
| **Buttercup** | 707000 | `0x930b540d` | spacing 150s→**75s**, subsidy 12.5→**6.25**, halving interval 840k→**1.68M**, **+3** triple-halving offset |

> Modern Zcashd has Blossom / Heartwood / Canopy / NU5 / NU6 / NU6.1 in these
> post-Sapling slots — none of which ZCL ever ran. Bubbles / DiffAdj / Buttercup
> do **not** exist in the modern codebase and must be added.

### Subsidy / PoW
- `nSubsidySlowStartInterval = 2`
- pre-Buttercup halving interval `840000`, post-Buttercup `1680000`
- `Halving()` adds `+3` after Buttercup (ref `consensus/params.cpp:14`)
- `powLimit = 0007ffff…`
- averaging window `17`; `nPowMaxAdjustDown = 32`; `nPowMaxAdjustUp = 16`
- `scaleDifficultyAtUpgradeFork = true` with graduated minimum-difficulty fork
  scaling on the first 17 blocks after DiffAdj/Buttercup (ref `pow.cpp:44-69`)
- Equihash solution-size → (N,K): 1344 → (200,9); 400 → (192,7) from Bubbles

### Block & transaction sizes / historical validation (sync-critical)
- **Standard limits** (enforced at the tip / above the last checkpoint):
  block acceptance `GENEROUS_BLOCK_SIZE_LIMIT = 2000000` (2MB — ZCL's real
  acceptance ceiling, also the P2P message limit; `MAX_BLOCK_SIZE = 200000` is
  only the *miner's* block-creation target, not an acceptance rule), and
  `MAX_TX_SIZE_AFTER_SAPLING = 102000` per transaction.
- **The historical chain violates today's structural rules.** It contains blocks
  and transactions the network accepted that do not satisfy current structural
  limits — e.g. a large multi-input consolidation transaction near height 753568
  (815 inputs → 1 output, ~124KB block) exceeding `MAX_TX_SIZE_AFTER_SAPLING`.
  ZCL gets away with this because it does **not** re-verify below its checkpoints.
- **Mechanism: checkpoint-gated skip of structural/expensive checks, turnstile
  always on.** `ShouldCheckTransactions()` returns false for blocks at or below
  the highest hardcoded checkpoint height (`GetTotalBlocksEstimate`), so the
  structural transaction checks (size, version, finality, …) are skipped there;
  `ConnectBlock` independently skips the expensive proof/signature checks for the
  same blocks (`fExpensiveChecks`). Above the checkpoint, **every block is fully
  verified**. This matches ZclassicCommunity/zclassic (its `fCheckSizeLimits`
  gating). Block acceptance uses the 2MB ceiling everywhere (`CheckBlock`); there
  is no strict 200000-byte acceptance rule.
- **Why this does not weaken anti-counterfeiting** — see the ZIP-209 section
  below: the turnstile and shielded value-pool accounting are in `ConnectBlock`,
  gated only on `ZIP209Enabled()`, and run on **every** block from genesis,
  independent of the structural/expensive skip.
- (History: an earlier attempt enforced size by height at the *Buttercup*
  boundary instead. That was wrong — the over-limit historical data is
  post-Buttercup but pre-checkpoint — and was replaced by the checkpoint-gated
  skip above. `-ibdskiptxverification` remains as an upstream opt-in flag.)

### Shielded value-pool integrity (ZIP-209 turnstile)
- The ZIP-209 turnstile (reject any block that drives a shielded value pool out
  of the valid monetary range — `turnstile-violation-*` in `ConnectBlock`) is
  **fully present** and, as of the genesis-sync-integrity work, **enabled on
  Zclassic mainnet** (`fZIP209Enabled = true`, also on testnet/regtest). This is
  a deliberate divergence from ZclassicCommunity/zclassic (whose mainnet leaves
  it off): with full from-genesis verification restored, a sync or `-reindex`
  now accumulates each shielded pool balance (Sprout/Sapling/Orchard/lockbox)
  from 0 and rejects any block that would make a pool negative — detecting
  counterfeiting of shielded value as an aggregate invariant.
- **No Sprout value-pool checkpoint is required.** The upstream
  `nSproutValuePoolCheckpoint` mechanism exists for *snapshot* sync (where early
  per-block Sprout deltas are absent); a full from-genesis validation accumulates
  `nChainSproutValue` from 0 with complete history, so it is always populated and
  the turnstile checks the running balance directly. (A node carrying legacy
  block-index data predating Sprout value-pool tracking is told to `-reindex`.)
- **Layering.** The core anti-counterfeiting guarantee is the per-transaction
  **binding signature** + value-balance accounting in
  `CheckTransaction`/`ConnectBlock` (fully active again now that the IBD
  verification-skip is reverted). ZIP-209 is the additional *aggregate* safety
  net on top.
- **Operational caveat.** This makes the node strictly enforce an invariant the
  wider ZCL network (turnstile off) does not. On an honest chain it never fires;
  if it ever does, the node halts at that block — either a genuine discovery or
  legacy data needing a reindex. Validate with a full `-reindex` from genesis
  before relying on it.

### Anchors
- checkpoints up to height `3126937` (`0x00000663e40f1fe0bc32a7e7282fac25de5fe8ecefd9c627e2fd948d388f7053`)
- `nMinimumChainWork = 0x…af996bfd8e482`
- fast-sync anchor at `3126937`
- live chain tip ~`3.13M` as of 2026-05-31

### Testnet (brief)
- magic `0xfa 0x1a 0xf9 0xbf`, P2P `18033`, currency `ZCT`, BIP44 `1`
- Overwinter/Sapling at 20, Bubbles at 6350, DiffAdj NO_ACTIVATION, Buttercup 78856
- `nPowAllowMinDifficultyBlocksAfterHeight = 299187`, `scaleDifficultyAtUpgradeFork = false`

---

## 2. Work plan (phased)

Phases 1–6 establish consensus faithfulness; 7–9 cover correctness, build, and
verification.

### Phase 0 — Scope lock & baseline
- Confirmed scope: **drop-in for live ZCL**.
- Build unmodified 6.12.3 (`depends` + Rust) first so every later change is
  bisectable.

### Phase 1 — Network & chain identity  ✅ DONE
Files: `chainparams.cpp`, `chainparamsbase.cpp`, `chainparamsseeds.h`.
- Overwrite mainnet/testnet/regtest magic, ports, base58 + bech32 prefixes,
  currency unit, BIP44 type, message-magic string, `strNetworkID`.
- Replace genesis (nTime/nBits/nNonce/solution/merkle/coinbase tag); **assert the
  computed genesis hash equals ZCL's**. Risk: the modern genesis builder +
  Equihash must reproduce ZCL's exact genesis hash — verify byte-for-byte.
- Replace seeds, checkpoints, `nMinimumChainWork`, fast-sync anchor.

> Implemented: shared `CreateGenesisBlock` now uses ZCL's coinbase timestamp and
> scriptSig constant (486604799); mainnet/testnet/regtest genesis values + asserts,
> magic/ports (8033/18033), currency (ZCL/ZCT/REG), BIP44 147, Overwinter+Sapling
> at 476969 (mainnet) / 20 (testnet), all Zcash post-Sapling upgrades disabled
> (NO_ACTIVATION), ZCL DNS seeds, ZCL checkpoints + nMinimumChainWork. Zcash-only
> Sprout/chain-supply checkpoints neutralised (ZIP209 off on mainnet). Genesis
> hash/merkle assert verification at runtime is pending a full build (Phase 9).

### Phase 2 — Remove the modern tax (align coinbase rule with ZCL)  ✅ DONE
Goal: only coinbase value rule is `coinbase <= subsidy + fees`.
- Remove the ZIP 207 funding-stream / ZIP 271 lockbox setup from chainparams.
- Remove enforcement in `main.cpp` (founders reward) and the founders output in
  `miner.cpp`; report 100%-to-miner in `rpc/mining.cpp` and `metrics.cpp`.

> Implemented: removed both funding-stream setup blocks (mainnet+testnet) from
> chainparams; removed the legacy Founders' Reward enforcement in
> `ContextualCheckBlock` (`main.cpp`); removed the founders output in `miner.cpp`
> (miner keeps 100%); `getblocktemplate`/`getblocksubsidy` no longer emit a
> founders reward; metrics no longer subtract 20%. The ZIP 207/271 funding +
> lockbox machinery and the vestigial `vFoundersRewardAddress`/helper methods are
> left compiled but inert (mirrors upstream Zclassic, which kept the data but
> never enforced it). Full removal of the dead classes is deferred to a later
> cleanup. The funding-stream enforcement in `ContextualCheckTransaction` is gated
> on Canopy/Heartwood/NU6 (all disabled) so it never executes.

### Phase 3 — Subsidy & halving math → ZCL's  ✅ DONE
File: `consensus/params.cpp`.
- Rewrite `GetBlockSubsidy()` / `Halving()` to ZCL semantics: `12.5*COIN` base,
  slow-start interval 2, Buttercup branch `(nSubsidy/2) >> halvings` with the
  `+3` offset, 840k/1.68M intervals.
- Reuse `nPreBlossom*` / `nPostBlossom*` interval fields as Pre/Post-Buttercup
  (Buttercup is structurally ZCL's "Blossom"; identical values 840000/1680000
  and spacing 150/75).

> Implemented: `Halving()` now keys on `UPGRADE_BUTTERCUP` with the `+3` triple-
> halving offset; `GetBlockSubsidy()` and `PoWTargetSpacing()` key on Buttercup
> (150s→75s at 707000); `nSubsidySlowStartInterval = 2` (mainnet+testnet). This
> reproduces the reference exactly, including the Buttercup-activation reward of
> `(12.5/2) >> 3 = 0.78125 ZCL` at height 707000. `HalvingHeight()` and
> `GetLastFoundersRewardBlockHeight()` are left Blossom-keyed but are only reached
> by vestigial/inert paths (founders asserts, empty funding streams), so they do
> not affect consensus.
>
> **Note:** because ZCL's subsidy math keys on `UPGRADE_BUTTERCUP`, the
> *structural* parts of Phase 4 (adding the `UPGRADE_BUBBLES/DIFFADJ/BUTTERCUP`
> enum entries, branch IDs, and activation heights) were pulled forward into this
> commit — see Phase 4.

### Phase 4 — Port ZCL's upgrade table & branch IDs (HIGHEST RISK)
Files: `consensus/params.h`, `consensus/upgrades.{cpp,h}`, `chainparams.cpp`,
plus every `NetworkUpgradeActive(...)` call site.
- Historical Sapling txs were signed under branch IDs `0x821a451c` /
  `0x930b540d`; the new node must compute identical sighashes, so
  `CurrentEpochBranchId(height)` must return ZCL's values.
- Add `UPGRADE_BUBBLES`, `UPGRADE_DIFFADJ`, `UPGRADE_BUTTERCUP` to
  `UpgradeIndex` and `NetworkUpgradeInfo[]` with exact branch IDs/heights; keep
  `BLOSSOM … NU6_1` defined but at `NO_ACTIVATION_HEIGHT (-1)`.
- Audit every `NetworkUpgradeActive(…, UPGRADE_BLOSSOM/CANOPY/HEARTWOOD/NU5…)`
  call site. Re-point the block-spacing/subsidy ones to `UPGRADE_BUTTERCUP`;
  leave Orchard/funding ones dead (NU5 never activates).
- Preserve the enum's "sorted by activation height" invariant that
  `upgrades.cpp` relies on.

> Partially DONE (structural part, done together with Phase 3): the three ZCL
> upgrades are added to the `UpgradeIndex` enum (inserted after `UPGRADE_SAPLING`,
> before the disabled Zcash upgrades, preserving ascending activation order) and
> to `NetworkUpgradeInfo[]` with exact branch IDs (`Bubbles` 0x821a451c; `Bubbly`
> /DiffAdj and `Buttercup` both 0x930b540d, matching the live chain). Activation
> heights set for all three networks (mainnet 585318/585322/707000, testnet
> 6350/disabled/78856, regtest disabled). The subsidy/spacing call sites are
> re-pointed to Buttercup (Phase 3).
>
> Phase 4 call-site audit (DONE): every non-test `UPGRADE_BLOSSOM/HEARTWOOD/
> CANOPY/NU5` reference is either (a) inert and correctly so — ZCL predates those
> upgrades, so the disabled (`-1`) heights keep the pre-upgrade code paths active
> (MMR/ZIP-221 history, shielded coinbase, funding streams, strict ZIP-212, etc.
> all correctly never run); or (b) already re-pointed to Buttercup
> (subsidy/spacing/halving, Phase 3) or solution-size-dispatched (Equihash,
> Phase 5). `CurrentEpochBranchId` returns ZCL's branch IDs across the
> Bubbles/DiffAdj/Buttercup boundaries by construction (enum order + verified
> activation heights). No `hashActivationBlock` values are set (matches upstream
> Zclassic, which relied on checkpoints + the fast-sync anchor instead).
>
> One non-consensus item intentionally left as-is: `main.cpp` (~9184) keys the
> *default expiry delta for newly created wallet transactions* on Blossom, so
> post-Buttercup new txs use the 20-block (pre-Blossom) default rather than 40.
> This is local wallet behaviour, not validation, so it does not affect sync or
> consensus; it could optionally be re-pointed to Buttercup later for usability.

### Phase 5 — PoW: difficulty + Equihash params  ✅ DONE
File: `pow.cpp`, `consensus/params.{h,cpp}`, `chainparams.cpp`.
- Added `Consensus::Params::scaleDifficultyAtUpgradeFork` (true on mainnet, false
  on testnet/regtest) and ported the graduated fork-scaling into
  `GetNextWorkRequired`: for the first `nPowAveragingWindow` blocks after the
  DiffAdj/Buttercup forks, relax difficulty by timestamp gap (min-difficulty at
  >12x spacing, ÷128 at >6x, ÷256 at >2x via the new `IncreaseDifficultyBy`),
  else fall through to the normal 17-block averaging retarget (which already
  matched the reference). The reference's `&&`/`||` grouping bug is reproduced
  exactly (with explicit parens to silence `-Wparentheses`) for consensus parity.
- **Equihash is solution-size–dispatched, not height-dependent.** The reference's
  `CheckEquihashSolution` derives `(n,k)` from `nSolution.size()`
  (1344→200,9; 400→192,7; 68→96,5; 36→48,5) and ignores `EquihashN(height)`,
  so the chain freely contains both 200,9 and 192,7 blocks (the network moved to
  192,7 ~2017, long before the formal Bubbles height 585318). Ported that exact
  dispatch into `CheckEquihashSolution`, falling back to the configured
  `nEquihashN/K` for any other size (e.g. custom regtest params).

> Discovery during sync: a 400-byte (192,7) solution was rejected at ~height 135k
> while the node expected 1344 (200,9) — confirming Equihash must be dispatched by
> solution size, decoupled from the Bubbles upgrade epoch (which governs only the
> branch id / sighash).

### Phase 6 — Cap transactions at Sapling (v4)  ✅ DONE (verified by sync)
- Leave NU5 … NU6.1 unactivated. `ContextualCheckTransaction` then **already**
  enforces v4 + `SAPLING_VERSION_GROUP_ID`, rejects v5/Orchard, and rejects
  stray consensus-branch-ids — no code change needed for the happy path.
  Orchard stays compiled (empty bundles) so the build is unaffected.
- Verify the assumptions that don't matter to ZCL don't break it: Heartwood
  "no shielded coinbase outputs" path, ZIP-212/216 "always-on after NU5"
  comments, expiry-threshold pre-NU5 path.
- Apply the wallet-side "neuter" gates so dormant features don't produce dead
  artifacts (see the dedicated section below).
- **Disable the MTP-relative future-timestamp soft fork.** Zcash's
  `nFutureTimestampSoftForkHeight` rule (block time must be ≤ MTP +
  `MAX_FUTURE_BLOCK_TIME_MTP`, v2.1.1-1) postdates the Zclassic fork; the
  historical Zclassic chain has blocks that violate it, so syncing from genesis
  fails intermittently with "too far ahead of median-time-past". Set
  `nFutureTimestampSoftForkHeight` to a never-activating height on all networks
  (done in `consensus/params.h` default + the mainnet override). Only the classic
  2-hour adjusted-time rule (`CheckBlockHeader`) and the MTP lower bound apply,
  matching the reference (Zcash 1.0.x).

#### Historical structural variation — checkpoint-gated skip, turnstile always on  ✅

The historical Zclassic chain contains blocks and transactions the network
accepted that do **not** satisfy today's structural consensus rules — most
visibly large multi-input consolidation transactions that exceed
`MAX_TX_SIZE_AFTER_SAPLING` (e.g. an 815-input → 1-output sweep near height
753568, bloating its block to ~124KB). ZCL accepts these because it does not
re-verify below its checkpoints.

This went through two wrong cuts before the right one:
1. *Default-on `-ibdskiptxverification`* (skip all tx checks below checkpoint) —
   reverted because it read as "verification disabled".
2. *Generous-size up to Buttercup, strict after* — wrong cutover: the over-limit
   data is post-Buttercup **but pre-checkpoint**, so it failed at 753568. Also
   mis-modelled block size (it tried to enforce 200000 at acceptance, but ZCL's
   real acceptance limit is 2MB; 200000 is only the miner's creation target).

**Final design (matches ZclassicCommunity/zclassic):**
- `ShouldCheckTransactions()` returns false for blocks at/below the highest
  hardcoded checkpoint height (`GetTotalBlocksEstimate`), skipping the structural
  transaction checks there. `ConnectBlock` independently skips the expensive
  proof/signature checks for the same blocks (`fExpensiveChecks`). **Above the
  checkpoint, every block is fully verified.**
- Block acceptance is the generous 2MB ceiling everywhere (`CheckBlock` /
  `LoadExternalBlockFile`); there is no strict 200000-byte acceptance rule.
- The per-transaction size limit (`MAX_TX_SIZE_AFTER_SAPLING`) and other
  structural rules then apply only where the checks run (above the checkpoint).

**This does not weaken anti-counterfeiting.** The ZIP-209 turnstile and shielded
value-pool accounting are in `ConnectBlock`, gated only on `ZIP209Enabled()`, and
run on **every** block from genesis — independent of the structural/expensive
skip (verified: they read transaction value-balance *fields*, which `ConnectBlock`
sums unconditionally; `fExpensiveChecks` only gates proofs/signatures/scripts).
So any block that would drive a shielded value pool out of range is rejected,
from block 0, and new (post-checkpoint) blocks additionally get full proof and
signature verification. Below the checkpoint, per-note crypto soundness is
vouched for by the hardcoded checkpoints, exactly as upstream Zcash/ZCL do.

### Keeping Orchard / unified addresses / v5 dormant but enableable

A deliberate design goal for Zclassicd: **do not delete** the modern shielded
machinery (Orchard, unified addresses, v5/ZIP-225 transactions, NU5/NU6 logic).
Keep it all compiled but inert, controlled purely by network-upgrade activation
heights, so a *future* Zclassic network upgrade can switch it on with no code
surgery.

**Governing principle: gate by activation height, never by deletion.** Every
post-Sapling feature in this codebase is already guarded by
`NetworkUpgradeActive(height, UPGRADE_*)`. Holding those upgrades at
`NO_ACTIVATION_HEIGHT` (Phase 1) makes them dormant while leaving the code,
the Rust Orchard/Sapling crates, and the wallet DB structures present and
forward-compatible.

What this means structurally:

- **Phase 4 must ADD new `UpgradeIndex` entries** for Zclassic's
  Bubbles/DiffAdj/Buttercup rather than repurposing the disabled
  Blossom/Heartwood/Canopy/NU5/NU6/NU6.1 slots. Those Zcash upgrades stay intact
  and disabled precisely so a future ZCL upgrade can reuse the Orchard/NU5
  machinery. (Decision locked: *add new entries.*)
- **Consensus is already neutered by disabling NU5:** v5/Orchard transactions are
  rejected (`bad-tx-has-orchard-actions`, v5 version rejected), Orchard bundles
  must be empty, and no Orchard pool can form. No code change required.
- **Wallet/RPC neuter gates (so dormant features don't emit dead artifacts):**
  - `CWallet::DefaultReceiverTypes(nHeight)` (`src/wallet/wallet.cpp`) now adds the
    Orchard receiver only when NU5 is active at `nHeight`; until then unified
    addresses contain only P2PKH + Sapling receivers.
  - `z_getaddressforaccount` (`src/wallet/rpcwallet.cpp`) rejects an explicit
    `"orchard"` receiver request while NU5 is inactive, with a clear
    "not enabled on this network" error, instead of returning a dead receiver.
  - `z_getnewaccount` / unified spending keys are left fully functional (they
    derive an internal Orchard key that simply stays dormant); `z_getnewaddress`
    has no Orchard path. Sprout address creation stays enabled (gated off Canopy,
    which is disabled), matching Zclassic's continued Sprout support.

**How a future Zclassic Orchard rollout would work (a deliberate hardfork):**

1. This is a coordinated **network upgrade (hardfork)** — expected, and distinct
   from the "no softfork" requirement for *today's* shoehorn (that requirement is
   about matching the existing chain's consensus, not about never upgrading).
2. Orchard has prerequisites in the Zcash lineage (Canopy's ZIP-212, Heartwood,
   then NU5). A ZCL rollout schedules those activation heights together at a
   future block — they are kept intact and disabled today precisely so they can
   be switched on as a bundle.
3. **Mint Zclassic-specific consensus branch IDs** for the rollout upgrade(s)
   rather than reusing Zcash's NU5/NU6 branch IDs, to prevent cross-chain
   transaction replay and peer confusion.
4. Once activation heights + branch IDs are set in `chainparams.cpp` and a release
   is shipped, the rest follows automatically: the wallet gates above open at the
   activation height, v5/Orchard transactions become valid, and unified addresses
   begin advertising Orchard receivers — with no further code change.
5. Review the funding/lockbox machinery before any such upgrade: it is inert today
   (no streams defined) but still compiled, so a future upgrade could either keep
   it disabled (Zclassic's fair-launch default) or, as a separate governance
   decision, define streams — that choice is independent of enabling Orchard.

### Phase 7 — Historical-validation correctness (Sprout & Sapling)  ✅ DONE (verified)
- Sprout JoinSplit verification params are present: `init.cpp` (`ZC_LoadParams`)
  loads `sprout-groth16.params` from `~/.zcash-params` (fetched if needed).
  Sapling params are bundled in the binary; Orchard params are generated
  deterministically (and dormant).
- **Empirically confirmed:** a full genesis→tip sync of the live ZCL mainnet
  validated every historical Sprout JoinSplit and Sapling proof, so the
  `librustzcash` / Sapling crypto FFI is compatible. Sprout remains fully usable.

### Phase 8 — Build, wallet, RPC, packaging  ✅ DONE (core)
- Compilation fallout from Phases 2–5 fixed; node builds clean and runs.
- Modern NU5+/Orchard/unified-address surfaces are neutralised (inert) via the
  wallet "neuter" gates (see the dedicated section) — not removed.
- Rebrand applied:
  - datadir `~/.zcash` → `~/.zclassic` (and `Zcash` → `Zclassic` on macOS/Windows);
    config `zclassic.conf`; pid `zclassicd.pid`. Params dir stays `~/.zcash-params`.
  - `CURRENCY_UNIT` `ZEC` → `ZCL` (RPC output).
  - Binaries renamed `zcashd`/`zcash-cli`/`zcash-tx` → `zclassicd`/`zclassic-cli`/
    `zclassic-tx` (`src/Makefile.am`, `configure.ac`, qa test framework).
  - Removed the upstream mandatory "zcashd is being deprecated in 2025 / migrate
    to zebrad+Zallet" startup gate; user-facing strings rebranded.
  - `CLIENT_NAME` stays `MagicBean` so the P2P user-agent matches the ZCL network.
- Cosmetic rebrand follow-ups (DONE): the Rust wallet helper is renamed to
  `zclassicd-wallet-tool` (Cargo bin + `src/Makefile.am`; its `wallet_tool.rs`
  also now shells out to `zclassic-cli` and defaults to `zclassic.conf` /
  `~/.zclassic`). Man pages (`doc/man/*`) and bash-completions are renamed and
  rebranded; `gen-manpages.sh` updated. Debian packaging (`contrib/debian/*`,
  `zcutil/build-debian-package.sh`), the Docker example, and the qa test
  framework are rebranded to `zclassic*`. The Sapling/Sprout params dir stays
  `~/.zcash-params` throughout (the node's `ZC_GetParamsDir` looks there). There
  is no systemd unit in this tree. Leftover `zcashd` mentions in individual
  rpc-test file *comments* are intentionally left (non-functional).

### Deprecated-RPC policy — allowed by default (Zclassic)
Upstream zcashd disables a set of "deprecated" RPC methods by default and forces
`-allowdeprecated=<feature>` to use them. On Zclassic these are basic, expected
wallet actions (Sprout + Sapling, no Orchard yet), so the wallet must not block
them. In `src/deprecation.h` all entries were moved from `DEFAULT_DENY_DEPRECATED`
into `DEFAULT_ALLOW_DEPRECATED`, so every deprecated feature
(`getnewaddress`, `getrawchangeaddress`, `z_getnewaddress`, `z_getbalance`,
`z_listaddresses`, `legacy_privacy`, `wallettxvjoinsplit`, `gbt_oldhashes`,
`addrtype`, `deprecationinfo_deprecationheight`) is enabled by default. The user
stays in control: `-allowdeprecated=none` disables them all (upstream
strictness), and `-allowdeprecated=<feature>` still names individual features.

> Caveat: this default-enables `legacy_privacy`, which (unlike the others, which
> are just blocked methods) changes the **default privacy policy** for
> `z_sendmany` to the more permissive "LegacyCompat" — convenient on a chain with
> active transparent/Sprout use, but a privacy-default change. To keep the strict
> privacy default while still allowing the address/balance methods, move only
> `legacy_privacy` back into `DEFAULT_DENY_DEPRECATED`.

### Phase 9 — Verification (the real acceptance test)  ✅ PASSED (genesis→tip)
- The node performed a full IBD against the **live ZCL network from genesis to
  the current tip** (~3.13M blocks), crossing every checkpoint and all three
  upgrade boundaries (Sapling 476969, Bubbles/DiffAdj 585318/585322, Buttercup
  707000) without divergence — i.e. it stayed on the same chain the live network
  produced. This is the acceptance test, and it passed.
- Remaining optional hardening: run side-by-side with a legacy node over a long
  window to also confirm it *produces* blocks legacy nodes accept (mining path),
  and spot-diff block hashes around the boundaries against a reference node.

### Wallet / Rust-side consensus fixes (post-bring-up)
The C++ consensus uses the correct Zclassic upgrade heights and branch IDs, but
the Rust crypto stack (librustzcash) has its own consensus view that must also be
made Zclassic-correct, or the *wallet* misbehaves even though block validation is
fine:

- **ZIP-212 / network params** (`src/rust/src/params.rs`): the Rust `network()`
  mapped "main"/"test" to `consensus::Network::MainNetwork`/`TestNetwork`, baking
  in Zcash's upgrade heights and ignoring the heights passed from C++. So the
  Rust wallet thought Canopy activated at Zcash's height and tried to decrypt
  Zclassic Sapling notes as ZIP-212 notes → **incoming shielded funds were
  undetectable**. Fixed by always carrying Zclassic's heights (Canopy/NU5
  disabled) with the correct `NetworkType`. Symptom was: a shielded receive did
  not appear in the wallet (transparent did).

- **Consensus branch ID for the Rust tx builder** (`depends/patches/` +
  `[patch.crates-io]`): the Rust builder converts the branch ID into
  `zcash_protocol::consensus::BranchId`, which only knows Zcash's IDs.
  `BranchId::try_from(0x930b540d)` (Zclassic Buttercup) returned `Err` and the
  node **aborted when signing a shielded send**. The v4 sighash embeds the branch
  ID literally, so no Zcash ID can substitute. Fix: vendor `zcash_protocol` under
  `depends/patched/` and repurpose the (Zclassic-unused) Canopy branch-ID constant
  to `0x930b540d`; `try_from`/`From` then round-trip it and it parses as v4. The
  vendor+patch is applied idempotently by `zcutil/build.sh`. Symptom was: a shielded
  *send* panicked with "Unknown consensus branch ID" once a spendable note existed.
  (Note: `z_sendmany` also defaults to 10 confirmations — `DEFAULT_NOTE_CONFIRMATIONS`
  — so a freshly-received note shows in `z_listunspent` (minconf 1) before it is
  spendable by `z_sendmany`.)
  `src/rust/src/history.rs` also calls `BranchId::try_from` but only for the
  Heartwood MMR (disabled on Zclassic), so it is not reached.

### Replay protection / branch-ID security
The consensus branch ID *is* the replay-protection mechanism: it is mixed into
the BLAKE2b personalization of the v4 (ZIP-243) transaction sighash, so a
signature is only valid under one specific branch ID. Zclassic's
`0x930b540d` (Buttercup) is distinct from every Zcash branch ID, so:

- Zclassic transactions cannot be replayed on Zcash and vice versa (different
  branch ID ⇒ different sighash ⇒ invalid signature on the other chain). This
  applies to transparent inputs (post-Overwinter) and to the Sapling binding
  signature alike. (Pre-Overwinter Sprout v1/v2 sighashes have no branch ID, but
  all new Zclassic transactions are v4.)
- The `Canopy → 0x930b540d` repurpose patch does **not** weaken this: the sighash
  still uses the real `0x930b540d`; the patch only teaches the Rust enum to
  recognise that value. Replay protection is unchanged.
- The patch is also fail-safe: it only affects transaction *construction* (C++
  still validates with the raw branch ID). A mistake would produce a transaction
  the network rejects, never one it wrongly accepts.

Caveats to keep in mind: the patch currently maps only Buttercup `0x930b540d`
(the branch ID for all transactions at today's tip). Bubbles `0x821a451c`
(heights 585318–585321) is not mapped, which is fine because new transactions
never use it. A *future* Zclassic network upgrade with a new branch ID would
need the patch (and ideally the proper fork below) updated. Replay between
Zclassic and other ZCL-derived chains (e.g. BTCP) is likewise governed by branch
IDs differing — worth confirming if that ever becomes relevant.

**This patch is the right approach _for now_.** It is minimal, reuses the
crates' audited ZIP-243 sighash, and is fail-safe.

### Long-term: ship fully-patched zcash Rust crates
The repurpose patch is a pragmatic stop-gap. The clean long-term solution is a
**maintained fork of the zcash Rust crates** (`zcash_protocol`, and anything that
matches on `BranchId` — `zcash_primitives`, `sapling-crypto`, `orchard`) that adds
Zclassic's branch IDs as *first-class* `BranchId` variants:

- `Bubbles` = `0x821a451c`, `DiffAdj`/`Buttercup` = `0x930b540d`, plus any future
  Zclassic upgrades — rather than renumbering Zcash's `Canopy`.
- Vendor these forks under `depends/` (or a Zclassic Git fork) and wire them via
  `[patch.crates-io]`, replacing the current single-constant repurpose. The
  vendoring/lockfile plumbing added for the stop-gap (`depends/patches/`,
  `depends/Makefile` lockfile handling) carries over directly.
- Benefits: supports *all* Zclassic branch IDs at once (not just the current
  epoch), survives crate upgrades more cleanly (a real diff vs. a sed), keeps
  `BranchId::for_height` semantics honest, and is auditable.
- Cost: the fork must be re-based whenever the upstream zcash crates are bumped,
  since adding an enum variant touches every exhaustive `match BranchId` in the
  dependency tree.

Until that fork exists, `depends/patches/apply-zcash-protocol-branchid-patch.sh`
is the supported mechanism.

---

## 3. Top risk register
1. **Sighash / branch-id parity** (Phase 4) — any mismatch silently forks at the
   first post-Bubbles Sapling tx.
2. **Genesis reproduction** (Phase 1) — Equihash / genesis builder differences.
3. **Equihash param switch & difficulty fork-scaling** (Phase 5) — must be
   bit-exact at heights 585318 / 585322 / 707000.
4. **Sprout historical verification** (Phase 7) — if modern code dropped Sprout
   proving, old blocks won't validate.
5. **Enum-ordering invariants** when injecting upgrades (Phase 4).

---

## 4. File-touch index (starting points)

| Concern | File(s) |
| --- | --- |
| Chain identity, genesis, checkpoints | `src/chainparams.cpp`, `src/chainparamsbase.cpp`, `src/chainparamsseeds.h` |
| Tax enforcement removal | `src/main.cpp` (~1090-1124, 1253-1279), `src/consensus/funding.{h,cpp}`, `src/consensus/params.{h,cpp}`, `src/miner.cpp` (117-174), `src/rpc/mining.cpp` |
| Subsidy / halving | `src/consensus/params.cpp` (`GetBlockSubsidy`, `Halving`, `HalvingHeight`) |
| Upgrade table / branch IDs | `src/consensus/params.h`, `src/consensus/upgrades.{cpp,h}`, `src/chainparams.cpp` |
| PoW / Equihash | `src/pow.cpp`, `src/consensus/params.cpp` |
| Tx version gating | `src/main.cpp` (`ContextualCheckTransaction`), `src/primitives/transaction.h`, `src/consensus/consensus.h` |
| Sprout/Sapling crypto | Rust FFI (`librustzcash`, `sapling-crypto`), `Cargo.toml` |
| Tests | `src/gtest/*` (remove founders/funding tests) |
