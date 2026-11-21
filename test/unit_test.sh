#!/usr/bin/env bash
# Builds and runs the unit tests for the modules' lower tiers.
# Usage: ./test/unit_test.sh
set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p debug

mapfile -t MODULES < <(find src -path src/platform -prune -o -path src/app -prune -o -type f -name '*.c' -print | sort)

gcc -std=c89 -Wall -Wextra -Wpedantic -g -O0 -Iinclude test/unit_test.c "${MODULES[@]}" -o debug/unit_test
./debug/unit_test
