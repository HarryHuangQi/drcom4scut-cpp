#!/usr/bin/env bash
# Install this source tree on supported native Ubuntu x86_64 or ARM64 systems.
# Does not enable the service or authenticate automatically.
set -Eeuo pipefail
trap 'printf "Installation failed at line %s. No automatic authentication was started.\n" "$LINENO" >&2' ERR

skip_deps=0
integration_test=0
for argument in "$@"; do
    case "$argument" in
        --skip-deps) skip_deps=1 ;;
        --integration-test) integration_test=1 ;;
        -h|--help)
            cat <<'HELP'
Usage: sudo bash install-ubuntu22.04.sh [--skip-deps] [--integration-test]

Requires Ubuntu 18.04, 20.04 or 22.04 on x86_64 or ARM64/aarch64.
Installs build dependencies, builds and tests the C++ client, then installs:
  /usr/local/bin/drcom4scut
  /etc/drcom4scut/config.conf            existing config is preserved
  /etc/systemd/system/drcom4scut.service

--skip-deps         Do not run apt; dependencies must already be installed.
--integration-test  Also run the isolated namespace/veth authentication test.

The service is NOT started or enabled. Configure credentials and the physical
Ethernet interface before starting it manually. No WSL is required.
HELP
            exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$argument" >&2; exit 2 ;;
    esac
done

[[ -f /etc/os-release ]] || { echo 'Linux /etc/os-release is required.' >&2; exit 1; }
# shellcheck source=/dev/null
source /etc/os-release
[[ ${ID:-} == ubuntu ]] || { echo 'This installer requires Ubuntu.' >&2; exit 1; }
case ${VERSION_ID:-} in
    18.04|20.04|22.04) ;;
    *) echo 'Supported Ubuntu versions are 18.04, 20.04 and 22.04.' >&2; exit 1 ;;
esac
machine_arch=$(uname -m)
[[ $machine_arch == x86_64 || $machine_arch == aarch64 ]] || {
    echo 'This installer supports x86_64 and ARM64/aarch64; 32-bit ARM/i386 are unsupported.' >&2
    exit 1
}

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
for required in main.cpp protocol.hpp CMakeLists.txt config.example drcom4scut.service tests/protocol_tests.cpp; do
    [[ -f "$source_dir/$required" ]] || { printf 'Missing source file: %s\n' "$required" >&2; exit 1; }
done
if (( EUID != 0 )); then
    command -v sudo >/dev/null || { echo 'Run this installer as root.' >&2; exit 1; }
    exec sudo -- bash "$source_dir/install-ubuntu22.04.sh" "$@"
fi

export DEBIAN_FRONTEND=noninteractive
if (( ! skip_deps )); then
    apt-get update
    apt-get install -y --no-install-recommends build-essential cmake libssl-dev
    if (( integration_test )); then
        apt-get install -y --no-install-recommends python3 iproute2
    fi
fi

build_dir="$source_dir/build-${VERSION_ID}-${machine_arch}"
cmake -H"$source_dir" -B"$build_dir" -DCMAKE_BUILD_TYPE=Release
# Keep peak memory modest, including on small campus PCs.
# Jetson Nano has limited memory, so use one compiler process.
cmake --build "$build_dir" -- -j1
(cd "$build_dir" && ctest --output-on-failure)
"$build_dir/drcom4scut" --help >/dev/null
if (( integration_test )); then
    python3 "$source_dir/tests/linux_integration.py" "$build_dir/drcom4scut"
fi

install -D -m 0755 "$build_dir/drcom4scut" /usr/local/bin/drcom4scut
install -d -m 0755 /etc/drcom4scut
if [[ ! -e /etc/drcom4scut/config.conf && ! -L /etc/drcom4scut/config.conf ]]; then
    install -m 0600 "$source_dir/config.example" /etc/drcom4scut/config.conf
else
    echo 'Preserved existing /etc/drcom4scut/config.conf'
fi
install -D -m 0644 "$source_dir/drcom4scut.service" /etc/systemd/system/drcom4scut.service
if [[ -d /run/systemd/system ]] && command -v systemctl >/dev/null; then
    systemctl daemon-reload
fi

cat <<'NEXT'
Installation complete. No service was started or enabled.

1. List interfaces:
   /usr/local/bin/drcom4scut --list-interfaces
2. Edit interface, authentication IPv4, username and password:
   sudo nano /etc/drcom4scut/config.conf
3. Test in the foreground (Ctrl+C exits):
   sudo /usr/local/bin/drcom4scut --config /etc/drcom4scut/config.conf --once
4. Optionally enable the background service after successful configuration:
   sudo systemctl enable --now drcom4scut.service
   journalctl -u drcom4scut.service -f

Use the physical Ethernet interface connected to the campus network. Set ip=
to the IPv4 embedded in authentication packets; it may differ from the local
DHCP address. The client does not run DHCP or change interface addresses.
NEXT
