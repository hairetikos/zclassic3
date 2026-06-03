#!/bin/sh

export LC_ALL=C
set -eu
set +x

cmd_pref() {
    if command -v "$2" >/dev/null; then
        eval "$1=$2"
    else
        eval "$1=$3"
    fi
}

# If a g-prefixed version of the command exists, use it preferentially.
gprefix() {
    cmd_pref "$1" "g$2" "$2"
}

gprefix READLINK readlink
cd "$(dirname "$("$READLINK" -f "$0")")/.."

# Fast incremental rebuild: `./zcutil/build.sh -rebuild [MAKEARGS...]` skips the
# dependency build, clean.sh, autogen and configure, and just runs `make` against
# the existing configuration. Use it after editing source when a full build would
# waste time. Run a full build first, and again whenever build flags or configure
# options change (so the new flags get baked into the Makefiles).
REBUILD=0
_rebuild_args=""
for arg in "$@"; do
    case "$arg" in
        -rebuild|--rebuild) REBUILD=1 ;;
        *) _rebuild_args="$_rebuild_args $arg" ;;
    esac
done
# Re-split on whitespace; build args (e.g. -j8, V=1) never contain spaces.
set -- $_rebuild_args

# Allow user overrides to $MAKE. Typical usage for users who need it:
#   MAKE=gmake ./zcutil/build.sh -j$(nproc)
if [ -z "${MAKE-}" ]; then
    MAKE="make"
fi

# Allow overrides to $BUILD and $HOST for porters. Most users will not need it.
#   BUILD=i686-pc-linux-gnu ./zcutil/build.sh
if [ -z "${BUILD-}" ]; then
    BUILD="$(./depends/config.guess)"
fi
if [ -z "${HOST-}" ]; then
    HOST="$BUILD"
fi

# Allow users to set arbitrary compile flags. Most users will not need this.
if [ -z "${CONFIGURE_FLAGS-}" ]; then
    # If the user did not set CONFIGURE_FLAGS, then use "--quiet" unless V=1 was given.
    CONFIGURE_FLAGS="--quiet"
    for arg in "$@"
    do
        if [ "$arg" = "V=1" ]; then
            CONFIGURE_FLAGS=""
        fi
    done
fi

if [ "$*" = '--help' ]
then
    cat <<EOF
Usage:

$0 --help
  Show this help message and exit.

$0 [ MAKEARGS... ]
  Build Zclassic and most of its transitive dependencies from
  source. MAKEARGS are applied to both dependencies and Zclassic itself.

$0 -rebuild [ MAKEARGS... ]
  Fast incremental rebuild: skip the dependency build, clean.sh, autogen and
  configure, and just run 'make' against the existing configuration. Use after
  editing source. Do a full build first (and again whenever build flags or
  configure options change).

  Pass flags to ./configure using the CONFIGURE_FLAGS environment variable.
  For example, to enable coverage instrumentation (thus enabling "make cov"
  to work), call:

      CONFIGURE_FLAGS="--enable-lcov --disable-hardening" ./zcutil/build.sh

  For verbose output, use:
      ./zcutil/build.sh V=1

  Performance tuning (applied to the main build only, not depends/):
      OPTIMIZE=1     ./zcutil/build.sh -j\$(nproc)   # -O3 -march=native
      MARCH_NATIVE=1 ./zcutil/build.sh -j\$(nproc)   # -march=native (recommended)
      O3=1           ./zcutil/build.sh -j\$(nproc)   # -O3
      LTO=1          ./zcutil/build.sh -j\$(nproc)   # -flto (link-time optimization)
      EXTRA_CXXFLAGS="..." ./zcutil/build.sh ...     # arbitrary extra flags
  A -march=native binary is CPU-specific (won't run on a different CPU family).
  LTO increases build time/memory; with GCC it auto-uses gcc-ar/ranlib/nm.
  Assertions are kept enabled (no -DNDEBUG) because they guard consensus.
EOF
    exit 0
fi

set -x

# Apply Zclassic's Rust crate patches (repurposes the Canopy consensus branch ID
# to Zclassic's Buttercup branch ID so the Rust tx builder can sign Zclassic
# transactions). Idempotent (no-op via a marker once vendored); needs no cargo or
# Rust toolchain (it fetches the exact locked crate tarball directly if needed),
# so it is safe to run first and fail fast. Must run before any cargo build,
# including -rebuild.
./depends/patches/apply-zcash-protocol-branchid-patch.sh

if [ "$REBUILD" = "1" ]; then
    echo "build.sh: -rebuild requested; skipping depends, clean.sh, autogen and configure."
    "$MAKE" "$@"
    exit 0
fi

eval "$MAKE" --version
as --version

case "$CONFIGURE_FLAGS" in
(*"--enable-debug"*)
    DEBUG=1
;;
(*)
    DEBUG=
;;esac

HOST="$HOST" BUILD="$BUILD" "$MAKE" "$@" -C ./depends/ DEBUG="$DEBUG"

if [ "${BUILD_STAGE:-all}" = "depends" ]
then
    exit 0
fi

./zcutil/clean.sh
./autogen.sh

# Optional performance tuning for the *main* zclassicd/zclassic-cli/zclassic-tx
# build (NOT the depends/ libraries, which are built separately above). Opt in
# with environment variables:
#
#   OPTIMIZE=1      ./zcutil/build.sh -j$(nproc)   # -O3 -march=native
#   MARCH_NATIVE=1  ./zcutil/build.sh -j$(nproc)   # -march=native only (recommended)
#   O3=1            ./zcutil/build.sh -j$(nproc)   # -O3 only
#   LTO=1           ./zcutil/build.sh -j$(nproc)   # -flto (link-time optimization)
#   EXTRA_CXXFLAGS="..." ./zcutil/build.sh ...     # arbitrary extra flags
#
# Notes:
#  * -march=native produces a CPU-specific binary (only runs on this CPU family)
#    and gives most of the real-world win (hashing, serialization, LevelDB).
#  * LTO increases build time and memory use; it needs -flto at both compile and
#    link time (handled below).
#  * Assertions are intentionally NOT disabled: they guard consensus-critical
#    invariants, so we never pass -DNDEBUG.
#  * config.site prepends the depends flags, so these appended flags win (the
#    last -O on the command line is the effective one).
PERF_CXXFLAGS=""
PERF_LDFLAGS=""
if [ "${OPTIMIZE-}" = "1" ]; then
    PERF_CXXFLAGS="-O3 -march=native"
fi
if [ "${O3-}" = "1" ]; then
    PERF_CXXFLAGS="$PERF_CXXFLAGS -O3"
fi
if [ "${MARCH_NATIVE-}" = "1" ]; then
    PERF_CXXFLAGS="$PERF_CXXFLAGS -march=native"
fi
if [ "${LTO-}" = "1" ]; then
    # -flto must be present at both compile and link time.
    PERF_CXXFLAGS="$PERF_CXXFLAGS -flto"
    PERF_LDFLAGS="$PERF_LDFLAGS -flto"
fi
PERF_CXXFLAGS="$PERF_CXXFLAGS ${EXTRA_CXXFLAGS-}"
# Trim leading/trailing whitespace.
PERF_CXXFLAGS="$(printf '%s' "$PERF_CXXFLAGS" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
PERF_LDFLAGS="$(printf '%s' "$PERF_LDFLAGS" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"

if [ -n "$PERF_CXXFLAGS" ]; then
    echo "build.sh: applying performance flags to the main build: $PERF_CXXFLAGS"
    export CXXFLAGS="${CXXFLAGS-} $PERF_CXXFLAGS"
    export CFLAGS="${CFLAGS-} $PERF_CXXFLAGS"
    if [ -n "$PERF_LDFLAGS" ]; then
        export LDFLAGS="${LDFLAGS-} $PERF_LDFLAGS"
    fi
    # GCC LTO writes LTO objects into the static libraries this build creates and
    # links (libbitcoin_*.a, libzcash.a, ...). The default ar/ranlib/nm don't
    # understand them and linking fails with "plugin needed to handle lto object".
    # Use the LTO-plugin-aware GCC wrappers when present and not already set, so
    # that configure bakes them into the Makefiles. (Clang/lld need no special
    # tools; clang users can override AR/RANLIB/NM, e.g. AR=llvm-ar.)
    if [ "${LTO-}" = "1" ] && command -v gcc-ar >/dev/null 2>&1; then
        export AR="${AR:-gcc-ar}"
        export RANLIB="${RANLIB:-gcc-ranlib}"
        export NM="${NM:-gcc-nm}"
        echo "build.sh: LTO enabled; using AR=$AR RANLIB=$RANLIB NM=$NM"
    fi
fi
CONFIG_SITE="$PWD/depends/$HOST/share/config.site" ./configure $CONFIGURE_FLAGS
"$MAKE" "$@"
