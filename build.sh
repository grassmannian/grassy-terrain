#!/usr/bin/env bash
# Convenience wrapper: enter the Nix dev shell and build.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec nix develop "$ROOT" -c "$ROOT/scripts/build.sh"
