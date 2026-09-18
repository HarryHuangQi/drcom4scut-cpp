#!/usr/bin/env bash
# ARM64-specific entry point. The shared installer contains the actual steps.
set -Eeuo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ $(uname -m) != aarch64 ]]; then
    echo 'This installer is for 64-bit ARM Ubuntu (uname -m must be aarch64).' >&2
    exit 1
fi
exec bash "$script_dir/install-ubuntu22.04.sh" "$@"
