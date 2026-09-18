#!/usr/bin/env bash
# x86_64-specific entry point. The shared installer contains the actual steps.
set -Eeuo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ $(uname -m) != x86_64 ]]; then
    echo 'This installer is for 64-bit x86 Ubuntu (uname -m must be x86_64).' >&2
    exit 1
fi
exec bash "$script_dir/install-ubuntu22.04.sh" "$@"
