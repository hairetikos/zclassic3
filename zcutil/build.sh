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
  Build Zcash and most of its transitive dependencies from
  source. MAKEARGS are applied to both dependencies and Zcash itself.

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
      EXTRA_CXXFLAGS="-flto" ./zcutil/build.sh ...   # arbitrary extra flags
  A -march=native binary is CPU-specific (won't run on a different CPU family).
  Assertions are kept enabled (no -DNDEBUG) because they guard consensus.
EOF
    exit 0
fi

set -x

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
#   EXTRA_CXXFLAGS="-flto" ./zcutil/build.sh ...   # arbitrary extra flags
#
# Notes:
#  * -march=native produces a CPU-specific binary (only runs on this CPU family)
#    and gives most of the real-world win (hashing, serialization, LevelDB).
#  * Assertions are intentionally NOT disabled: they guard consensus-critical
#    invariants, so we never pass -DNDEBUG.
#  * config.site prepends the depends flags, so these appended flags win (the
#    last -O on the command line is the effective one).
PERF_CXXFLAGS=""
if [ "${OPTIMIZE-}" = "1" ]; then
    PERF_CXXFLAGS="-O3 -march=native"
fi
if [ "${O3-}" = "1" ]; then
    PERF_CXXFLAGS="$PERF_CXXFLAGS -O3"
fi
if [ "${MARCH_NATIVE-}" = "1" ]; then
    PERF_CXXFLAGS="$PERF_CXXFLAGS -march=native"
fi
PERF_CXXFLAGS="$PERF_CXXFLAGS ${EXTRA_CXXFLAGS-}"
# Trim leading/trailing whitespace.
PERF_CXXFLAGS="$(printf '%s' "$PERF_CXXFLAGS" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"

if [ -n "$PERF_CXXFLAGS" ]; then
    echo "build.sh: applying performance flags to the main build: $PERF_CXXFLAGS"
    CONFIG_SITE="$PWD/depends/$HOST/share/config.site" \
        CXXFLAGS="${CXXFLAGS-} $PERF_CXXFLAGS" \
        CFLAGS="${CFLAGS-} $PERF_CXXFLAGS" \
        ./configure $CONFIGURE_FLAGS
else
    CONFIG_SITE="$PWD/depends/$HOST/share/config.site" ./configure $CONFIGURE_FLAGS
fi
"$MAKE" "$@"
