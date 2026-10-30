#!/usr/bin/env bash
# Run the headless binding smoke tests inside the dev shell.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec nix develop "$ROOT" -c bash -c \
  "JANET_PATH='$ROOT/build' LD_LIBRARY_PATH='$ROOT/vendor/ghostty/lib' janet '$ROOT/test/bindings.janet'"
