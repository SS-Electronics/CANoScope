#!/usr/bin/env bash
# install_dependencies.sh – Install build and runtime dependencies
# for CANoScope on Debian/Ubuntu-based systems.
#
# Usage:  sudo ./scripts/install_dependencies.sh

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

# ------------------------------------------------------------------ #
# Check root                                                           #
# ------------------------------------------------------------------ #
if [ "$EUID" -ne 0 ]; then
    error "Please run as root:  sudo $0"
fi

# ------------------------------------------------------------------ #
# Detect package manager                                               #
# ------------------------------------------------------------------ #
if command -v apt-get &>/dev/null; then
    PKG_MGR="apt-get"
elif command -v dnf &>/dev/null; then
    PKG_MGR="dnf"
elif command -v pacman &>/dev/null; then
    PKG_MGR="pacman"
else
    error "Unsupported package manager. Install manually: gcc, make, libgtk-3-dev, libglib2.0-dev, can-utils"
fi

info "Package manager: $PKG_MGR"

# ------------------------------------------------------------------ #
# Install packages                                                     #
# ------------------------------------------------------------------ #
case "$PKG_MGR" in
apt-get)
    apt-get update -qq
    apt-get install -y \
        build-essential \
        pkg-config \
        libgtk-3-dev \
        libglib2.0-dev \
        can-utils \
        linux-headers-$(uname -r) || true
    ;;
dnf)
    dnf install -y \
        gcc make pkgconfig \
        gtk3-devel glib2-devel \
        can-utils \
        kernel-devel
    ;;
pacman)
    pacman -Sy --noconfirm \
        base-devel \
        gtk3 glib2 \
        can-utils
    ;;
esac

info "System packages installed."

# ------------------------------------------------------------------ #
# Load kernel modules for SocketCAN                                    #
# ------------------------------------------------------------------ #
info "Loading CAN kernel modules..."
for mod in can can_raw can_dev vcan; do
    if modprobe "$mod" 2>/dev/null; then
        info "  Loaded: $mod"
    else
        warn "  Could not load $mod (may already be built-in)"
    fi
done

# ------------------------------------------------------------------ #
# Make modules persistent                                              #
# ------------------------------------------------------------------ #
MODS_FILE=/etc/modules-load.d/can.conf
if [ ! -f "$MODS_FILE" ]; then
    cat > "$MODS_FILE" <<'EOF'
# SocketCAN modules – loaded at boot for CANoScope
can
can_raw
can_dev
vcan
EOF
    info "Created $MODS_FILE (modules will load at boot)"
else
    info "$MODS_FILE already exists – skipping"
fi

# ------------------------------------------------------------------ #
# Create a virtual CAN interface for testing                           #
# ------------------------------------------------------------------ #
info "Setting up virtual CAN interface vcan0 for testing..."
if ! ip link show vcan0 &>/dev/null; then
    ip link add dev vcan0 type vcan
    ip link set up vcan0
    info "vcan0 created and brought up."
else
    ip link set up vcan0 2>/dev/null || true
    info "vcan0 already exists."
fi

# ------------------------------------------------------------------ #
# PEAK PCAN USB driver (optional, for real hardware)                   #
# ------------------------------------------------------------------ #
info ""
info "Optional: PEAK-System PCAN hardware support"
info "  For PCAN-USB and other PEAK interfaces on Linux:"
info "  • Kernel ≥ 3.2: driver included (peak_usb module)"
info "  • Check: lsmod | grep peak"
info "  • Load:  modprobe peak_usb"
info "  • Or use the proprietary driver from:"
info "    https://www.peak-system.com/fileadmin/media/linux/index.php"
info ""

# ------------------------------------------------------------------ #
# Summary                                                              #
# ------------------------------------------------------------------ #
info "All dependencies installed successfully."
info ""
info "Quick start:"
info "  Build:     make"
info "  Run test:  ./build/canoscope   (uses vcan0 by default)"
info "  Send test: cansend vcan0 123#DEADBEEF"
info ""
