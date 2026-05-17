#!/bin/sh
set -eu

REPO_URL="${MUGVIDEO_REPO_URL:-https://github.com/komagata/mugvideo.git}"
PREFIX="${PREFIX:-/usr/local}"

need() {
  command -v "$1" >/dev/null 2>&1
}

sudo_cmd() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    sudo "$@"
  fi
}

install_deps() {
  if need apt-get; then
    sudo_cmd apt-get update
    sudo_cmd apt-get install -y \
      build-essential git pkg-config libgtk-4-dev libgstreamer1.0-dev \
      libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-base \
      gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
      gstreamer1.0-libav
    return
  fi

  if need dnf; then
    sudo_cmd dnf install -y \
      gcc make git pkgconf-pkg-config gtk4-devel gstreamer1-devel \
      gstreamer1-plugins-base-devel gstreamer1-plugins-good \
      gstreamer1-plugins-bad-free gstreamer1-libav
    return
  fi

  if need zypper; then
    sudo_cmd zypper --non-interactive install \
      gcc make git pkgconf-pkg-config gtk4-devel gstreamer-devel \
      gstreamer-plugins-base-devel gstreamer-plugins-good \
      gstreamer-plugins-bad gstreamer-plugins-libav
    return
  fi

  echo "Unsupported package manager. Install GTK4, GStreamer, git, pkg-config, gcc, and make, then run make install." >&2
  exit 1
}

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT INT TERM

install_deps
git clone --depth 1 "$REPO_URL" "$tmpdir/mugvideo"
cd "$tmpdir/mugvideo"
make
sudo_cmd make PREFIX="$PREFIX" install

echo "mugvideo installed to $PREFIX/bin/mugvideo"
