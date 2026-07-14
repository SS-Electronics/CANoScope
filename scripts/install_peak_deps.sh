#!/usr/bin/env bash
# install_peak_deps.sh - Install PEAK/PCAN Linux support for CANoScope.
#
# Default mode prepares the in-kernel SocketCAN PEAK drivers used by CANoScope.
# Optional mode downloads and builds PEAK-System's PCAN-Linux package for
# chardev/PCAN-Basic or netdev driver use.
#
# Usage:
#   sudo ./scripts/install_peak_deps.sh
#   sudo ./scripts/install_peak_deps.sh --with-pcan-driver --driver-mode netdev
#   sudo ./scripts/install_peak_deps.sh --with-pcan-driver --driver-mode chardev --dkms

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

PCAN_DRIVER_URL=${PCAN_DRIVER_URL:-https://www.peak-system.com/quick/PCAN-Linux-Driver}
PEAK_SRC_PARENT=${PEAK_SRC_PARENT:-/usr/local/src}

WITH_PCAN_DRIVER=0
DRIVER_MODE=chardev
USE_DKMS=0
INSTALL_PACKAGES=1
PERSIST_MODULES=1

usage()
{
    cat <<EOF
Usage: sudo $0 [OPTIONS]

Prepare Linux support for PEAK-System PCAN hardware.

Default behavior:
  - install SocketCAN/CAN tooling and kernel headers
  - load in-kernel PEAK SocketCAN modules: peak_usb, peak_pci, peak_pciefd
  - persist those modules with /etc/modules-load.d/peak-can.conf

Options:
  --with-pcan-driver       Download/build PEAK's PCAN-Linux driver package.
                           Use this only for PCAN-Basic/chardev, old kernels,
                           or when mainline peak_* modules are insufficient.
  --driver-mode MODE       PCAN package build mode: chardev or netdev.
                           Default: chardev.
  --chardev                Shortcut for --driver-mode chardev.
  --netdev                 Shortcut for --driver-mode netdev.
  --dkms                   Use the PCAN package install_with_dkms target.
  --no-package-install     Skip OS package installation.
  --no-persist             Do not create /etc/modules-load.d/peak-can.conf.
  -h, --help               Show this help.

Environment:
  PCAN_DRIVER_URL          Override PEAK package URL.
                           Default: $PCAN_DRIVER_URL
  PEAK_SRC_PARENT          Source extraction parent directory.
                           Default: $PEAK_SRC_PARENT
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --with-pcan-driver)
        WITH_PCAN_DRIVER=1
        ;;
    --driver-mode)
        [ "$#" -ge 2 ] || error "--driver-mode requires chardev or netdev"
        DRIVER_MODE=$2
        shift
        ;;
    --chardev)
        DRIVER_MODE=chardev
        ;;
    --netdev)
        DRIVER_MODE=netdev
        ;;
    --dkms)
        USE_DKMS=1
        ;;
    --no-package-install)
        INSTALL_PACKAGES=0
        ;;
    --no-persist)
        PERSIST_MODULES=0
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        error "Unknown option: $1"
        ;;
    esac
    shift
done

case "$DRIVER_MODE" in
chardev|netdev) ;;
*) error "--driver-mode must be chardev or netdev" ;;
esac

if [ "$EUID" -ne 0 ]; then
    error "Please run as root: sudo $0"
fi

detect_pkg_mgr()
{
    if command -v apt-get >/dev/null 2>&1; then
        PKG_MGR=apt-get
    elif command -v dnf >/dev/null 2>&1; then
        PKG_MGR=dnf
    elif command -v pacman >/dev/null 2>&1; then
        PKG_MGR=pacman
    else
        error "Unsupported package manager. Install build tools, curl, tar, kmod, iproute2, can-utils, and kernel headers manually."
    fi
    info "Package manager: $PKG_MGR"
}

require_cmd()
{
    command -v "$1" >/dev/null 2>&1 || error "Required command not found: $1"
}

install_optional_pkg()
{
    local pkg=$1
    case "$PKG_MGR" in
    apt-get)
        apt-get install -y "$pkg" || warn "Could not install optional package: $pkg"
        ;;
    dnf)
        dnf install -y "$pkg" || warn "Could not install optional package: $pkg"
        ;;
    pacman)
        pacman -S --noconfirm --needed "$pkg" || warn "Could not install optional package: $pkg"
        ;;
    esac
}

install_packages()
{
    if [ "$INSTALL_PACKAGES" -eq 0 ]; then
        info "Skipping OS package installation."
        return
    fi

    info "Installing PEAK/SocketCAN build and runtime dependencies..."
    case "$PKG_MGR" in
    apt-get)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y \
            build-essential \
            ca-certificates \
            curl \
            g++ \
            iproute2 \
            kmod \
            libpopt-dev \
            make \
            pciutils \
            pkg-config \
            tar \
            usbutils \
            can-utils \
            ethtool
        install_optional_pkg "linux-headers-$(uname -r)"
        if [ "$USE_DKMS" -eq 1 ]; then
            install_optional_pkg dkms
        fi
        ;;
    dnf)
        dnf install -y \
            ca-certificates \
            can-utils \
            curl \
            ethtool \
            gcc \
            gcc-c++ \
            iproute \
            kmod \
            make \
            pciutils \
            pkgconf-pkg-config \
            popt-devel \
            tar \
            usbutils \
            xz
        install_optional_pkg "kernel-devel-$(uname -r)"
        install_optional_pkg kernel-headers
        if [ "$USE_DKMS" -eq 1 ]; then
            install_optional_pkg dkms
        fi
        ;;
    pacman)
        pacman -Sy --noconfirm --needed \
            base-devel \
            ca-certificates \
            can-utils \
            curl \
            ethtool \
            iproute2 \
            kmod \
            pciutils \
            popt \
            tar \
            usbutils \
            xz
        install_optional_pkg linux-headers
        if [ "$USE_DKMS" -eq 1 ]; then
            install_optional_pkg dkms
        fi
        ;;
    esac
}

print_peak_kernel_config()
{
    info "Checking kernel PEAK driver config..."
    if [ -r "/boot/config-$(uname -r)" ]; then
        grep '^CONFIG_CAN_PEAK' "/boot/config-$(uname -r)" || \
            warn "No CONFIG_CAN_PEAK* entries found in /boot/config-$(uname -r)."
    elif [ -r /proc/config.gz ] && command -v zgrep >/dev/null 2>&1; then
        zgrep '^CONFIG_CAN_PEAK' /proc/config.gz || \
            warn "No CONFIG_CAN_PEAK* entries found in /proc/config.gz."
    else
        warn "Kernel config not readable; skipping CONFIG_CAN_PEAK* check."
    fi
}

load_socketcan_modules()
{
    info "Loading SocketCAN and PEAK kernel modules..."
    local modules=(can can_raw can_dev peak_usb peak_pci peak_pciefd)
    local loaded=()
    local err_file

    err_file=$(mktemp)

    for mod in "${modules[@]}"; do
        if modprobe "$mod" 2>"$err_file"; then
            info "  Loaded or available: $mod"
            loaded+=("$mod")
        else
            warn "  Could not load $mod: $(tr '\n' ' ' < "$err_file")"
        fi
        : > "$err_file"
    done
    rm -f "$err_file"

    if [ "$PERSIST_MODULES" -eq 1 ] && [ "${#loaded[@]}" -gt 0 ]; then
        local mods_file=/etc/modules-load.d/peak-can.conf
        {
            echo "# SocketCAN + PEAK-System modules for CANoScope"
            printf '%s\n' "${loaded[@]}"
        } > "$mods_file"
        info "Created $mods_file"
    elif [ "$PERSIST_MODULES" -eq 0 ]; then
        info "Module persistence disabled."
    else
        warn "No modules loaded; not creating a modules-load file."
    fi
}

show_can_interfaces()
{
    info "Current CAN interfaces:"
    if ! command -v ip >/dev/null 2>&1; then
        warn "ip command not found; install iproute2 and run: ip link show type can"
        return
    fi

    local ifaces
    ifaces=$(ip link show type can 2>/dev/null || true)
    if [ -n "$ifaces" ]; then
        printf '%s\n' "$ifaces"
    else
        warn "No CAN network interfaces are currently visible."
        warn "Connect PEAK hardware, then check again with: ip link show type can"
    fi
}

install_peak_driver_package()
{
    info "Downloading PEAK PCAN-Linux driver package..."
    require_cmd curl
    require_cmd make
    require_cmd tar

    if [ "$USE_DKMS" -eq 1 ]; then
        require_cmd dkms
    fi

    mkdir -p "$PEAK_SRC_PARENT"

    local tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT

    local archive="$tmp/peak-linux-driver.tar.gz"

    curl -fsSL "$PCAN_DRIVER_URL" -o "$archive"
    tar -xzf "$archive" -C "$tmp"

    local src_dir
    src_dir=$(find "$tmp" -maxdepth 1 -type d -name 'peak-linux-driver-*' | head -n 1)
    [ -n "$src_dir" ] || error "Downloaded archive did not contain peak-linux-driver-*."

    local dst="$PEAK_SRC_PARENT/$(basename "$src_dir")"
    if [ -d "$dst" ]; then
        warn "$dst already exists; reusing it."
    else
        mv "$src_dir" "$dst"
    fi
    rm -rf "$tmp"
    trap - EXIT

    info "Building PEAK PCAN-Linux package in $dst ($DRIVER_MODE mode)..."
    local make_vars=()
    case "$DRIVER_MODE" in
    chardev)
        make_vars+=(NET=NO_NETDEV_SUPPORT)
        ;;
    netdev)
        make_vars+=(NET=NETDEV_SUPPORT)
        ;;
    esac
    make -C "$dst" "${make_vars[@]}" clean all

    if [ "$USE_DKMS" -eq 1 ]; then
        info "Installing PEAK PCAN-Linux package with DKMS..."
        make -C "$dst" "${make_vars[@]}" install_with_dkms
    else
        info "Installing PEAK PCAN-Linux package..."
        make -C "$dst" "${make_vars[@]}" install
    fi

    depmod -a || true
    if modprobe pcan 2>/dev/null; then
        info "Loaded proprietary pcan module."
    else
        warn "Could not load proprietary pcan module immediately. Reboot or inspect dmesg if hardware is connected."
    fi
}

detect_pkg_mgr
install_packages
print_peak_kernel_config
load_socketcan_modules

if [ "$WITH_PCAN_DRIVER" -eq 1 ]; then
    warn "Building PEAK's proprietary PCAN-Linux package can replace/blacklist mainline peak_* behavior."
    warn "CANoScope uses SocketCAN; prefer the in-kernel peak_* modules unless you specifically need PCAN-Basic/chardev or PEAK's netdev package."
    install_peak_driver_package
fi

show_can_interfaces

info ""
info "PEAK/PCAN setup finished."
info "For CANoScope, connect using the SocketCAN interface shown by:"
info "  ip link show type can"
info "Then select that interface in CANoScope, for example can0 at 500000 bps."
