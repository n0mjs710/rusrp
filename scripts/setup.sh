#!/bin/sh
# Fetch vendored dependencies.
# Run once after cloning the repository.
set -e

TOMLC99_URL="https://raw.githubusercontent.com/cktan/tomlc99/master"
VENDOR_DIR="$(dirname "$0")/../vendor/tomlc99"

echo "Fetching tomlc99..."
curl -fsSL "$TOMLC99_URL/toml.h" -o "$VENDOR_DIR/toml.h"
curl -fsSL "$TOMLC99_URL/toml.c" -o "$VENDOR_DIR/toml.c"
echo "Done. You can now run: meson setup build && ninja -C build"
