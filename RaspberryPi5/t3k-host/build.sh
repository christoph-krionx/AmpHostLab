#!/usr/bin/env bash
set -euo pipefail

# Builds t3k-host natively on the Pi (Raspberry Pi OS 64-bit).
# Clones the TONE3000 plugin next to this script on first run.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
T3K_PLUGIN_DIR="${T3K_PLUGIN_DIR:-${SCRIPT_DIR}/tone3000-plugin}"
# Commit the host was written against; override with T3K_REF=main to track upstream.
T3K_REF="${T3K_REF:-ab5e5ae}"
BUILD_DIR="${SCRIPT_DIR}/build"

log() {
  echo
  echo "==> $1"
}

require_arm64() {
  if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "Fehler: Dieses Script erwartet ein 64-bit ARM System."
    echo "Gefunden: $(uname -m)"
    exit 1
  fi
}

install_dependencies() {
  log "Build-Abhaengigkeiten installieren"
  sudo apt update
  sudo apt install -y \
    build-essential cmake git pkg-config \
    libasound2-dev \
    libfreetype6-dev libfontconfig1-dev \
    libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
    libxi-dev libxinerama-dev libxrandr-dev libxrender-dev
}

fetch_plugin() {
  if [[ ! -d "${T3K_PLUGIN_DIR}/.git" ]]; then
    log "TONE3000 Plugin klonen"
    git clone https://github.com/tone-3000/tone3000-plugin.git "${T3K_PLUGIN_DIR}"
  fi
  log "Plugin auf ${T3K_REF} setzen"
  git -C "${T3K_PLUGIN_DIR}" fetch origin
  git -C "${T3K_PLUGIN_DIR}" checkout "${T3K_REF}"
  git -C "${T3K_PLUGIN_DIR}" submodule update --init --recursive
}

build_host() {
  log "t3k-host bauen"
  cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DT3K_PLUGIN_DIR="${T3K_PLUGIN_DIR}"
  cmake --build "${BUILD_DIR}" --target t3k-host -j"$(nproc)"
}

main() {
  require_arm64
  install_dependencies
  fetch_plugin
  build_host

  log "Fertig"
  find "${BUILD_DIR}" -type f -name t3k-host -perm -u+x
}

main "$@"
