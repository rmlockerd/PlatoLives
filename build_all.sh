#!/usr/bin/env bash
set -e

echo "================================================================="
echo ">>> PLATOLIVES - MULTI-PLATFORM BUILD PIPELINE"
echo "================================================================="

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"
mkdir -p build

# 1. BUILD NATIVA macOS (GUI + Headless + Console)
echo ""
echo "[1/3] Compilazione nativa macOS (Clang / CMake)..."
cmake -B build > /dev/null
make -C build -j$(sysctl -n hw.ncpu)
echo ">>> Esecuzione test unitari (ctest)..."
ctest --test-dir build --output-on-failure

# 2. CROSS-COMPILAZIONE LINUX ARM64 (aarch64-linux-musl)
echo ""
echo "[2/3] Cross-compilazione Linux ARM64 statica (Zig cc)..."
zig cc -target aarch64-linux-musl -std=c11 -O2 -s -Iinclude \
    src/console_main.c \
    src/console_runner.c \
    src/plato_protocol.c \
    src/plato_terminal.c \
    src/plato_transport.c \
    src/plato_graphics.c \
    src/plato_font.c \
    src/plato_framebuffer.c \
    src/plato_ringbuf.c \
    src/plato_keyboard.c \
    -pthread -static \
    -o build/PlatoLives-linux-arm64

# 3. CROSS-COMPILAZIONE LINUX INTEL (x86_64-linux-musl)
echo ""
echo "[3/3] Cross-compilazione Linux Intel x86_64 statica (Zig cc)..."
zig cc -target x86_64-linux-musl -std=c11 -O2 -s -Iinclude \
    src/console_main.c \
    src/console_runner.c \
    src/plato_protocol.c \
    src/plato_terminal.c \
    src/plato_transport.c \
    src/plato_graphics.c \
    src/plato_font.c \
    src/plato_framebuffer.c \
    src/plato_ringbuf.c \
    src/plato_keyboard.c \
    -pthread -static \
    -o build/PlatoLives-linux-x86_64

echo ""
echo "================================================================="
echo ">>> BUILD COMPLETATA CON SUCCESSO PER TUTTI I TARGET!"
echo "================================================================="
file build/PlatoLives.app/Contents/MacOS/PlatoLives
file build/PlatoLives-linux-arm64
file build/PlatoLives-linux-x86_64
echo ""
ls -lh build/PlatoLives.app/Contents/MacOS/PlatoLives build/PlatoLives-linux-arm64 build/PlatoLives-linux-x86_64
echo "================================================================="
