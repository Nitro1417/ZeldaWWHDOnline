#!/usr/bin/env bash
# install.sh — Build and install the WWHD Online relay server on a Linux VPS.
#
# Tested on Ubuntu 22.04/24.04 and Debian 12 (ARM64 and x86_64).
# Run as a non-root user with sudo access:
#
#   curl -fsSL https://raw.githubusercontent.com/Nitro1417/ZeldaWWHDOnline/master/relay/install.sh | bash
#
# What this does:
#   1. Installs build tools (cmake, g++, git, ninja)
#   2. Clones the repo and builds the relay binary
#   3. Installs it to /usr/local/bin/wwhd_relay
#   4. Creates a systemd service so it starts on boot
#   5. Opens port 7778/udp in ufw (if installed)
#
# After install, your relay address is this machine's public IP, port 7778.
set -e

REPO="https://github.com/Nitro1417/ZeldaWWHDOnline.git"
PORT="${RELAY_PORT:-7778}"
INSTALL_BIN="/usr/local/bin/wwhd_relay"
SERVICE_NAME="wwhd-relay"
BUILD_DIR="/tmp/wwhd-relay-build"

echo "==> Installing build tools..."
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    cmake ninja-build g++ git ca-certificates

echo "==> Cloning repo..."
rm -rf "$BUILD_DIR"
git clone --depth 1 "$REPO" "$BUILD_DIR/src"

echo "==> Building relay server..."
cmake -S "$BUILD_DIR/src/relay" \
      -B "$BUILD_DIR/build" \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR/build"

echo "==> Installing binary..."
sudo install -m 755 "$BUILD_DIR/build/wwhd_relay" "$INSTALL_BIN"

echo "==> Creating systemd service..."
sudo tee /etc/systemd/system/${SERVICE_NAME}.service > /dev/null <<EOF
[Unit]
Description=WWHD Online Relay Server
After=network.target

[Service]
ExecStart=${INSTALL_BIN} ${PORT}
Restart=on-failure
RestartSec=5
# Run as nobody for security
User=nobody
Group=nogroup

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE_NAME"
sudo systemctl restart "$SERVICE_NAME"

echo "==> Opening firewall port ${PORT}/udp..."
if command -v ufw &>/dev/null; then
    sudo ufw allow "${PORT}/udp"
fi

# Oracle Cloud: also needs the OS-level iptables rule
# (Oracle's VMs have an extra iptables chain that blocks by default)
if sudo iptables -L INPUT -n 2>/dev/null | grep -q "oci-"; then
    echo "==> Detected Oracle Cloud — adding iptables rule..."
    sudo iptables -I INPUT -p udp --dport "$PORT" -j ACCEPT
    # Make it persistent across reboots
    sudo apt-get install -y --no-install-recommends iptables-persistent
    sudo netfilter-persistent save
fi

echo ""
echo "====================================================="
echo " WWHD Online relay is running on port ${PORT}/udp"
echo ""
echo " Host:   --online-relay-create <this-server-ip> ${PORT}"
echo " Join:   --online-relay-join   <this-server-ip> ${PORT} <6-char-code>"
echo ""
echo " Status: sudo systemctl status ${SERVICE_NAME}"
echo " Logs:   sudo journalctl -u ${SERVICE_NAME} -f"
echo "====================================================="
