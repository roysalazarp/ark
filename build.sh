#!/usr/bin/env bash
set -euo pipefail

rm -rf debug*
mkdir -p debug

SOURCE=$(find ./src -name "*.cpp")

echo "Building..."
g++ -std=c++17 -Wall -Wextra -Wpedantic -g "$SOURCE" -o debug/server

echo "Build successful"