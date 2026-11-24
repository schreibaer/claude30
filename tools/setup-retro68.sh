#!/usr/bin/env bash
# Install the Retro68 68k toolchain that builds Claude30.
#
# Fast path: unpack a prebuilt archive from the cache folder (seconds).
# Slow path: build the pinned Retro68 commit from source (about half an
# hour), then save the result to the cache folder so the next run is fast.
#
#   tools/setup-retro68.sh              # install (unpack or build), then print the cmake line
#   tools/setup-retro68.sh --rebuild    # ignore the cache, build from source and refresh the cache
#   tools/setup-retro68.sh --no-save    # build if needed but don't write to the cache
#   tools/setup-retro68.sh --resume     # after a failed build: keep the built gcc, redo the rest
#
# Settings (environment):
#   RETRO68_PREFIX     where the toolchain goes      (default /opt/Retro68-build)
#   RETRO68_CACHE_DIR  where cached archives live    (default /mnt/project-files/toolchain)
#   RETRO68_SRC        where to clone Retro68        (default $RETRO68_PREFIX-src)
#
# The archive holds absolute paths from the build, so it is only used when it
# unpacks to the same RETRO68_PREFIX it was built in. It is named after the
# Retro68 commit, the OS and the CPU, and a .txt manifest next to it records
# how it was made. Supported hosts: Debian/Ubuntu (apt). Elsewhere, install
# the packages listed below yourself and the script does the rest.
set -euo pipefail

# Retro68 has no tagged releases. Change this pin (and client/README.md)
# to move to a newer Retro68; the new commit gets its own cache archive.
RETRO68_REPO=https://github.com/autc04/Retro68.git
RETRO68_COMMIT=a4a389c41e90b1095a40ccf13f28678b6cfbba86

PREFIX=${RETRO68_PREFIX:-/opt/Retro68-build}
CACHE_DIR=${RETRO68_CACHE_DIR:-/mnt/project-files/toolchain}
SRC=${RETRO68_SRC:-$PREFIX-src}

# Packages needed to run the toolchain, and the extra ones needed to build it.
RUN_PKGS=(cmake make python3 libgmp10 libmpfr6 libmpc3 zlib1g
          libboost-filesystem-dev libboost-program-options-dev libboost-wave-dev libboost-thread-dev)
# Retro68's own tests need more of boost than the toolchain does, so the
# build uses libboost-all-dev as Retro68's README suggests.
BUILD_PKGS=(git gcc g++ bison flex texinfo ruby libgmp-dev libmpfr-dev libmpc-dev zlib1g-dev
            libboost-all-dev)

rebuild=0
save=1
resume=0
for arg in "$@"; do
  case $arg in
    --rebuild) rebuild=1 ;;
    --no-save) save=0 ;;
    --resume) resume=1; rebuild=1 ;;
    -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

log() { printf '\n==> %s\n' "$*"; }

os_id=unknown
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  os_id=$(. /etc/os-release && echo "${ID}${VERSION_ID:-}")
fi
host_tag="$(uname -s | tr 'A-Z' 'a-z')-$(uname -m)-$os_id"
# The prefix is part of the key because the archive is not relocatable.
prefix_tag=$(printf '%s' "$PREFIX" | tr -c 'A-Za-z0-9._-' '_' | sed 's/^_*//')
name="retro68-m68k-${RETRO68_COMMIT:0:12}-${host_tag}-${prefix_tag}"
archive="$CACHE_DIR/$name.tar.gz"
manifest="$CACHE_DIR/$name.txt"
stamp="$PREFIX/.retro68-commit"
toolchain_file="$PREFIX/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake"

apt_install() {
  command -v apt-get >/dev/null || { echo "apt-get not found; install these yourself: $*" >&2; return 0; }
  local missing=()
  for p in "$@"; do dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p"); done
  [ ${#missing[@]} -eq 0 ] && return 0
  log "Installing packages: ${missing[*]}"
  local sudo=""
  [ "$(id -u)" -ne 0 ] && sudo=sudo
  $sudo apt-get update -qq || true
  DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y -qq --no-install-recommends "${missing[@]}"
}

done_msg() {
  cat <<EOF

Retro68 ${RETRO68_COMMIT:0:12} is ready in $PREFIX.
Build Claude30 from the repository root with:

  cmake -S client -B build-68k -DCMAKE_TOOLCHAIN_FILE=$toolchain_file
  cmake --build build-68k
EOF
}

# 1. Already installed at the pinned commit?
if [ $rebuild -eq 0 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$RETRO68_COMMIT" ] && [ -f "$toolchain_file" ]; then
  apt_install "${RUN_PKGS[@]}"
  log "Retro68 already installed"
  done_msg
  exit 0
fi

# 2. Unpack the cached archive.
if [ $rebuild -eq 0 ] && [ -f "$archive" ]; then
  apt_install "${RUN_PKGS[@]}"
  log "Unpacking cached toolchain $archive"
  if [ -f "$archive.sha256" ]; then
    want=$(cut -d' ' -f1 "$archive.sha256")
    got=$(sha256sum "$archive" | cut -d' ' -f1)
    if [ "$want" != "$got" ]; then
      echo "Checksum mismatch for $archive (want $want, got $got). Run with --rebuild." >&2
      exit 1
    fi
  fi
  rm -rf "$PREFIX"
  mkdir -p "$(dirname "$PREFIX")"
  tar -xzf "$archive" -C "$(dirname "$PREFIX")"
  [ -f "$toolchain_file" ] || { echo "Archive did not contain $toolchain_file" >&2; exit 1; }
  echo "$RETRO68_COMMIT" > "$stamp"
  done_msg
  exit 0
fi

# 3. Build from source.
[ $rebuild -eq 1 ] || echo "No cached toolchain at $archive; building from source (about 30 minutes)."
apt_install "${RUN_PKGS[@]}" "${BUILD_PKGS[@]}"

log "Fetching Retro68 $RETRO68_COMMIT into $SRC"
if [ ! -d "$SRC/.git" ]; then
  git clone --quiet "$RETRO68_REPO" "$SRC"
fi
git -C "$SRC" fetch --quiet origin "$RETRO68_COMMIT" 2>/dev/null || git -C "$SRC" fetch --quiet origin
git -C "$SRC" checkout --quiet --force "$RETRO68_COMMIT"
git -C "$SRC" submodule update --init --recursive --quiet

log "Building the 68k toolchain in $PREFIX"
build_opts=(--no-ppc --no-carbon --multiversal)
if [ $resume -eq 1 ] && [ -d "$PREFIX/gcc-build" ]; then
  build_opts+=(--skip-thirdparty)
else
  rm -rf "$PREFIX"
  mkdir -p "$PREFIX"
fi
started=$(date +%s)
( cd "$PREFIX" && "$SRC/build-toolchain.bash" "${build_opts[@]}" )
echo "$RETRO68_COMMIT" > "$stamp"
minutes=$(( ($(date +%s) - started) / 60 ))

if [ $save -eq 1 ]; then
  if mkdir -p "$CACHE_DIR" 2>/dev/null && [ -w "$CACHE_DIR" ]; then
    log "Saving the toolchain to $archive"
    # Only the installed toolchain is kept; the build trees are not needed.
    tmp="$CACHE_DIR/.$name.$$.tar.gz"
    tar -czf "$tmp" -C "$(dirname "$PREFIX")" "$(basename "$PREFIX")/toolchain" "$(basename "$PREFIX")/.retro68-commit"
    mv -f "$tmp" "$archive"
    ( cd "$CACHE_DIR" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256" )
    cat > "$manifest" <<EOF
Retro68 toolchain cache for Claude30
repo:     $RETRO68_REPO
commit:   $RETRO68_COMMIT
options:  build-toolchain.bash --no-ppc --no-carbon --multiversal
targets:  m68k-apple-macos only
prefix:   $PREFIX  (unpack here only; paths are absolute)
host:     $host_tag
gcc:      $("$PREFIX/toolchain/bin/m68k-apple-macos-gcc" --version | head -1)
built:    $(date -u +%Y-%m-%dT%H:%MZ) in ${minutes} min
sha256:   $(cut -d' ' -f1 "$archive.sha256")
made by:  tools/setup-retro68.sh in https://github.com/schreibaer/claude30
EOF
  else
    echo "Cache folder $CACHE_DIR is not writable; the toolchain was not saved." >&2
  fi
fi

done_msg
