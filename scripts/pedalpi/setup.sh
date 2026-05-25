#!/usr/bin/env bash
set -euo pipefail

PIPEDAL_VERSION="2.0.102"
PIPEDAL_PACKAGE="pipedal_${PIPEDAL_VERSION}_arm64.deb"
PIPEDAL_URL="https://github.com/rerdavies/pipedal/releases/download/v${PIPEDAL_VERSION}/${PIPEDAL_PACKAGE}"
DOWNLOAD_DIR="${HOME}/Downloads"

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
  log "System aktualisieren"
  sudo apt update
  sudo apt upgrade -y

  log "Benötigte Tools installieren"
  sudo apt install -y wget ca-certificates
}

download_pipedal() {
  log "Download-Verzeichnis vorbereiten"
  mkdir -p "$DOWNLOAD_DIR"
  cd "$DOWNLOAD_DIR"

  if [[ -f "$PIPEDAL_PACKAGE" ]]; then
    log "PiPedal Paket existiert bereits: $PIPEDAL_PACKAGE"
  else
    log "PiPedal herunterladen"
    wget "$PIPEDAL_URL"
  fi
}

install_pipedal() {
  log "PiPedal installieren"
  cd "$DOWNLOAD_DIR"

  sudo apt install -y "./$PIPEDAL_PACKAGE"
}

main() {
  require_arm64
  install_dependencies
  download_pipedal
  install_pipedal

  log "Fertig"
  echo "PiPedal wurde installiert."
}

main "$@"