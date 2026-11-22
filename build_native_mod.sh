#!/usr/bin/env bash
# build_native_mod.sh — Build wwhd_online.dll using the WWHD SDK's bundled toolchain.
#
# Usage:
#   ./build_native_mod.sh [path/to/WindWakerHD-vX.Y.Z-windows-x86_64]
#
# The script finds llvm-mingw in the SDK, clones ENet if needed,
# compiles net_mod.cpp + transport sources, and links wwhd_online.dll.
# Output: wwhd_online.dll (copy to your WWHD mods/ folder alongside manifest.json)

set -euo pipefail

SDK="${1:-/d/downloads/WindWakerHD-v0.5.1-devel.23159601-windows-x86_64}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NET_SRC="$SCRIPT_DIR/runtime-ext/src/net"
MOD_SRC="$SCRIPT_DIR/runtime-ext/src/native_mod"
MOD_INC="$SCRIPT_DIR/runtime-ext/include"

# ── Toolchain ──────────────────────────────────────────────────────────────
TOOLCHAIN="$SDK/tools/llvm-mingw-20260922-ucrt-x86_64/bin"
CXX="$TOOLCHAIN/x86_64-w64-mingw32-g++"
if [ ! -x "$CXX" ]; then
    # Try alternate name without date stamp
    TOOLCHAIN="$(find "$SDK/tools" -name 'x86_64-w64-mingw32-g++' 2>/dev/null | head -1)"
    CXX="$TOOLCHAIN"
fi
if [ ! -x "$CXX" ]; then
    echo "ERROR: Could not find x86_64-w64-mingw32-g++ in $SDK/tools" >&2
    exit 1
fi
echo "Compiler: $CXX"

# ── ENet ──────────────────────────────────────────────────────────────────
ENET_DIR="$SCRIPT_DIR/enet"
if [ ! -d "$ENET_DIR" ]; then
    echo "Cloning ENet 1.3.17..."
    git clone --depth 1 --branch v1.3.17 \
        https://github.com/lsalzman/enet.git "$ENET_DIR"
fi

# Compile ENet (single-file style)
ENET_OBJ="$ENET_DIR/enet.o"
if [ ! -f "$ENET_OBJ" ] || [ "$ENET_DIR/include/enet/enet.h" -nt "$ENET_OBJ" ]; then
    echo "Compiling ENet..."
    "$CXX" -c -O2 -DENET_DLL \
        -I"$ENET_DIR/include" \
        "$ENET_DIR/callbacks.c" "$ENET_DIR/compress.c" \
        "$ENET_DIR/host.c" "$ENET_DIR/list.c" "$ENET_DIR/packet.c" \
        "$ENET_DIR/peer.c" "$ENET_DIR/protocol.c" "$ENET_DIR/win32.c" \
        -o /dev/null 2>/dev/null || true
    # Compile all .c into a single archive
    mkdir -p "$ENET_DIR/build"
    for f in callbacks compress host list packet peer protocol win32; do
        "$CXX" -c -O2 -I"$ENET_DIR/include" "$ENET_DIR/$f.c" \
               -o "$ENET_DIR/build/$f.o"
    done
    ar rcs "$ENET_DIR/build/libenet.a" "$ENET_DIR/build/"*.o
fi

# ── Compile & link ─────────────────────────────────────────────────────────
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra"
INCLUDES="-I$MOD_INC -I$NET_SRC -I$ENET_DIR/include -I$SDK/sdk/include"

echo "Compiling WWHD Online native mod..."

SRCS=(
    "$MOD_SRC/net_mod.cpp"
    "$NET_SRC/net.cpp"
    "$NET_SRC/session.cpp"
    "$NET_SRC/stun.cpp"
    "$NET_SRC/p2p.cpp"
)

OBJS=()
for src in "${SRCS[@]}"; do
    obj="${src%.cpp}.o"
    echo "  $src"
    "$CXX" $CXXFLAGS $INCLUDES -c "$src" -o "$obj"
    OBJS+=("$obj")
done

echo "Linking wwhd_online.dll..."
"$CXX" -shared -o "$SCRIPT_DIR/wwhd_online.dll" \
    "${OBJS[@]}" \
    "$ENET_DIR/build/libenet.a" \
    -lws2_32 -lwinmm -lwinhttp -lcrypt32 \
    -Wl,--out-implib,"$SCRIPT_DIR/wwhd_online.lib"

echo ""
echo "Build successful: wwhd_online.dll"
echo ""
echo "Install:"
echo "  1. Copy wwhd_online.dll and manifest.json to your WWHD mods/ folder."
echo "  2. Launch the game with --online-* args.  Examples:"
echo ""
echo "  P2P host (recommended):"
echo "    WindWakerHD.exe --online-p2p-create https://wwhd-signal.YOUR.workers.dev"
echo ""
echo "  P2P join:"
echo "    WindWakerHD.exe --online-p2p-join https://wwhd-signal.YOUR.workers.dev ABCXYZ"
echo ""
echo "  Direct host (open port 7777):"
echo "    WindWakerHD.exe --online-host 7777"
echo ""
echo "  Direct join:"
echo "    WindWakerHD.exe --online-join YOUR_IP 7777"
