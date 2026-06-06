# Building Zclassic for Windows (cross-compile from Linux)

This guide builds the Windows binaries — `zclassicd.exe`, `zclassic-cli.exe`,
`zclassic-tx.exe` (and `zclassicd-wallet-tool.exe`) — **on a Linux machine** using
the MinGW-w64 cross toolchain, smoke-tests them under **Wine**, and packages them
for release. This is the same `depends`-based cross-compile flow Zcash uses for its
Windows releases; the produced binaries are byte-for-byte the same consensus node
as the Linux build, just compiled for `x86_64-pc-windows-gnu`.

> **Target:** 64-bit Windows (`x86_64-w64-mingw32`). 32-bit is not supported here.
>
> **You build on Linux. You do not need a Windows machine to compile.** A real
> Windows box (or a Windows VM) is still recommended for final acceptance testing;
> Wine is good for smoke tests, not a substitute for the real OS.

---

## 0. What you need

- A 64-bit **Debian 12+ / Ubuntu 22.04+** host (these are the officially supported
  build platforms). Other distros work but you're on your own for package names.
- **~8 GB RAM minimum** (the Rust + Boost + node build is memory-hungry; 16 GB is
  comfortable), several CPU cores, and **~15 GB free disk**.
- A working **internet connection** for the first build: `depends/` downloads and
  builds Rust (incl. the `x86_64-pc-windows-gnu` std), Boost, libevent, etc., and
  the consensus branch-ID patch fetches one crate from crates.io.
- Time: the first full cross-build is **slow** (often 1–3 hours depending on the
  machine) because it builds the entire dependency tree from source. Subsequent
  builds reuse `depends/` and are much faster.

---

## 1. Install the cross toolchain and build tools

```bash
sudo apt update
sudo apt install -y \
    build-essential pkg-config m4 autoconf automake libtool bsdmainutils \
    git curl wget unzip python3 \
    mingw-w64 g++-mingw-w64-x86-64 \
    clang lld
```

- `mingw-w64` + `g++-mingw-w64-x86-64` — the Windows cross compiler.
- `clang` / `lld` — the Windows link step uses the LLVM linker
  (`depends/hosts/mingw32.mk` sets `-fuse-ld=lld`). `depends/` builds its own
  Clang/LLD, but having the system ones present avoids surprises.
- `curl` — required by the consensus branch-ID patch (`zcutil/build.sh` runs it
  first; it downloads the exact pinned `zcash_protocol` crate).

## 2. Select the POSIX-threads MinGW variant (important)

MinGW-w64 ships two threading models. Zclassic uses C++ `std::thread` /
`std::mutex`, which only exist in the **POSIX** variant. Switch both the compiler
and linker driver to `-posix`:

```bash
sudo update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix
sudo update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
```

Verify:

```bash
x86_64-w64-mingw32-g++ --version      # should print a version, no error
update-alternatives --display x86_64-w64-mingw32-g++ | grep 'link currently'
```

If you skip this step the build fails later with errors about `std::mutex` /
`std::thread` being undefined.

## 3. Get the source

```bash
git clone https://github.com/hairetikos/zclassic3.git
cd zclassic3
git checkout <the-branch-or-tag-you-are-releasing>
```

## 4. Cross-compile

`zcutil/build.sh` understands the `HOST` triple and drives the whole flow
(branch-ID patch → build `depends/` for the target → `configure` → `make`):

```bash
HOST=x86_64-w64-mingw32 ./zcutil/build.sh -j"$(nproc)"
```

Notes:

- **Do NOT add `MARCH_NATIVE=1` / `OPTIMIZE=1` for binaries you intend to
  publish.** Those enable `-march=native`, which bakes in the *build machine's*
  CPU features and produces a binary that crashes with "illegal instruction" on
  other CPUs. The default mingw release flags (`-O3`, see
  `depends/hosts/mingw32.mk`) are already applied and are portable. Only use the
  perf flags for a private build you run on the same machine.
- The first run builds the whole dependency tree under
  `depends/x86_64-w64-mingw32/`. If you only want to (re)build the dependencies
  first, you can run `BUILD_STAGE=depends HOST=x86_64-w64-mingw32 ./zcutil/build.sh -j"$(nproc)"`.
- After editing source you can re-link quickly with
  `HOST=x86_64-w64-mingw32 ./zcutil/build.sh -rebuild -j"$(nproc)"` (skips the
  depends rebuild and `configure`). Do a full build first.

## 5. Collect and check the artifacts

The Windows executables land in `src/`:

```bash
ls -lh src/zclassicd.exe src/zclassic-cli.exe src/zclassic-tx.exe
file src/zclassicd.exe          # -> PE32+ executable (console) x86-64, for MS Windows
```

(`zclassicd-wallet-tool.exe` is produced by the Rust build under
`target/x86_64-pc-windows-gnu/release/`.)

Strip the debug symbols for distribution (smaller downloads):

```bash
x86_64-w64-mingw32-strip src/zclassicd.exe src/zclassic-cli.exe src/zclassic-tx.exe
```

These are **static** Windows binaries (the depends build links Boost, libevent,
OpenSSL, etc. statically), so they do **not** need extra MSVC/MinGW runtime DLLs.

## 6. Runtime data Windows users will need

- **Sapling parameters are compiled into the binary** — nothing to download for
  Sapling.
- **Sprout parameters are not.** For full validation of historical Sprout
  JoinSplits, `sprout-groth16.params` must be present on the *end user's* machine
  at:

  ```
  %APPDATA%\ZcashParams\sprout-groth16.params
  ```
  i.e. `C:\Users\<name>\AppData\Roaming\ZcashParams\sprout-groth16.params`,
  downloaded from <https://download.z.cash/downloads/sprout-groth16.params>.

  (During a sync-from-genesis the pre-checkpoint skip means Sprout proofs below
  the last checkpoint aren't re-verified, but ship/install this file anyway for
  correctness post-checkpoint and for `-ibdskiptxverification=0` users.)
- The node's **data directory** on Windows is `%APPDATA%\Zclassic`
  (`C:\Users\<name>\AppData\Roaming\Zclassic`), where `zclassic.conf`,
  `blocks/`, `chainstate/`, `wallet.dat`, etc. live.

## 7. Smoke-test under Wine

Wine is fine for "does it load and run?", not for full network/consensus testing.

```bash
sudo apt install -y wine64
winecfg            # once, to initialize the default ~/.wine prefix (set to Win10)

# Smoke tests:
wine src/zclassic-cli.exe --version
wine src/zclassicd.exe --version
wine src/zclassicd.exe --help | head -40
```

To actually start syncing under Wine, place the Sprout params inside the Wine
prefix's APPDATA and launch the daemon:

```bash
WP="$HOME/.wine/drive_c/users/$USER/AppData/Roaming"
mkdir -p "$WP/ZcashParams"
cp /path/to/sprout-groth16.params "$WP/ZcashParams/"     # if you have it
mkdir -p "$WP/Zclassic"
printf 'rpcuser=test\nrpcpassword=test\n' > "$WP/Zclassic/zclassic.conf"

wine src/zclassicd.exe -printtoconsole
# in another terminal:
wine src/zclassic-cli.exe getinfo
```

**Wine caveats:** networking, Tor/SOCKS, and some filesystem timing behave
differently under Wine than on real Windows. Treat a clean `--version`/`getinfo`
and a few blocks of sync as a smoke test only. **Do final acceptance on real
Windows.**

## 8. Package for release

Produce a clean, checksummed zip:

```bash
VER=$(src/zclassic-cli.exe --version 2>/dev/null | head -1 | grep -oE 'v[0-9].*' || echo v3.0.0-sigma)
DIST="zclassic-${VER}-win64"
mkdir -p "dist/$DIST"
cp src/zclassicd.exe src/zclassic-cli.exe src/zclassic-tx.exe "dist/$DIST/"
cp README.md COPYING "dist/$DIST/" 2>/dev/null || true

# A short Windows-user note:
cat > "dist/$DIST/WINDOWS-README.txt" <<'TXT'
Zclassic for Windows (64-bit)
- Run zclassicd.exe to start the node; control it with zclassic-cli.exe.
- Data dir: %APPDATA%\Zclassic  (zclassic.conf goes here)
- For full Sprout validation, download sprout-groth16.params from
  https://download.z.cash/downloads/sprout-groth16.params
  and place it in %APPDATA%\ZcashParams\
TXT

( cd dist && zip -r "$DIST.zip" "$DIST" && sha256sum "$DIST.zip" > "$DIST.zip.sha256" )
ls -lh "dist/$DIST.zip" "dist/$DIST.zip.sha256"
```

Publish the `.zip` and its `.sha256` as a GitHub Release asset (Releases → Draft a
new release → attach files). Always publish the SHA-256 so users can verify the
download. Reproducibility note: for *officially reproducible* release binaries,
Zcash uses the Gitian descriptors in `contrib/gitian-descriptors/gitian-win.yml`;
this manual flow is the practical path for community/test builds.

---

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| `undefined reference to std::mutex/std::thread/...` | MinGW not on the POSIX threads variant — redo **Step 2** (`update-alternatives --set ... -posix` for both `gcc` and `g++`). |
| `cannot find -llld` / `unknown argument -fuse-ld=lld` | Install `lld` (Step 1); ensure the `depends/` build finished (it provides the LLVM linker on `PATH` via `config.site`). |
| Branch-ID patch error at the very start | Need `curl` (or `wget`) and crates.io reachable; see `depends/patches/apply-zcash-protocol-branchid-patch.sh`. |
| `rust-std ... x86_64-pc-windows-gnu` download fails | First-build network issue; re-run — `depends` resumes. The target is wired in `depends/packages/native_rust.mk`. |
| Build OOM-killed | Lower parallelism (`-j2`), add swap, or use a bigger machine — the Rust/Boost steps peak high. |
| `.exe` runs on your PC but crashes on others ("illegal instruction") | You built with `-march=native`. Rebuild **without** `MARCH_NATIVE/OPTIMIZE` (Step 4). |
| Antivirus / SmartScreen flags the unsigned `.exe` | Expected for unsigned community binaries. Publish SHA-256 sums; consider code-signing for official releases. |
| Wine: node starts but can't fetch Sprout proofs | Put `sprout-groth16.params` in the prefix's `...\AppData\Roaming\ZcashParams\` (Step 7). |

## Notes

- **Consensus is identical** to the Linux build — this is purely a different
  compilation target. A Windows node and a Linux node are the same network peer.
- Keep the cross toolchain selection (POSIX threads) consistent across rebuilds;
  switching variants mid-tree can produce link errors.
- For day-to-day testing prefer the native Linux build; reach for the Windows
  cross-build when you're cutting a Windows release.
