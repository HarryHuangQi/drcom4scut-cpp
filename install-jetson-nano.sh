#!/usr/bin/env bash
# Jetson Nano / JetPack 4 normally uses aarch64 Ubuntu 18.04 (L4T R32).
set -Eeuo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
[[ $(uname -m) == aarch64 ]] || {
    echo 'Jetson Nano must report aarch64. This script does not support armv7l/armhf.' >&2
    exit 1
}
[[ -f /etc/os-release ]] || { echo 'Linux /etc/os-release is required.' >&2; exit 1; }
# shellcheck source=/dev/null
source /etc/os-release
[[ ${ID:-} == ubuntu ]] || { echo 'Jetson Nano installer requires Ubuntu/Jetson Linux.' >&2; exit 1; }
case ${VERSION_ID:-} in
    18.04|20.04|22.04) ;;
    *) echo 'Supported Jetson Nano Ubuntu versions: 18.04, 20.04 and 22.04.' >&2; exit 1 ;;
esac
if [[ ! -f /etc/nv_tegra_release ]]; then
    echo 'Notice: /etc/nv_tegra_release was not found; continuing for compatible aarch64 Ubuntu.' >&2
fi
exec bash "$script_dir/install-ubuntu22.04.sh" "$@"
