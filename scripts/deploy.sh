#!/bin/sh
# Anonymous FTP helpers for the PS4. Override with PS4_HOST / PS4_PORT.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
HOST=${PS4_HOST:-192.168.1.136}
PORT=${PS4_PORT:-2121}
BASE="ftp://$HOST:$PORT"
ELF="$ROOT/build/savedrive.elf"
PROBE="$ROOT/src/probe/probe.elf"
OUT="$ROOT/build"

usage() {
  echo "usage: $0 {deploy|probe|fetch}" >&2
  exit 2
}

put() {
  echo "upload $1 -> $BASE$2"
  curl -sS -T "$1" "$BASE$2"
}

get() {
  echo "download $BASE$1 -> $2"
  curl -sS -o "$2" "$BASE$1"
}

case "${1:-}" in
deploy)
  [ -f "$ELF" ] || { echo "missing $ELF (run make -C src)" >&2; exit 1; }
  put "$ELF" /data/payloads/savedrive.elf
  ;;
probe)
  [ -f "$PROBE" ] || { echo "missing $PROBE (run make -C src/probe)" >&2; exit 1; }
  put "$PROBE" /data/payloads/sdprobe.elf
  ;;
fetch)
  mkdir -p "$OUT"
  get /data/savedrive/run.log "$OUT/run.log" || echo "run.log not available"
  get /data/savedrive/probe.txt "$OUT/probe.txt" || echo "probe.txt not available"
  ;;
*)
  usage
  ;;
esac
