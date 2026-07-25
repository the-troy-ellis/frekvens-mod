#!/usr/bin/env bash
# Host-side tests for the parts of the firmware that are pure logic.
# No hardware, no PlatformIO, no network — just a C++ compiler.
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-g++}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

echo "building raid_engine_test…"
# -Istub supplies the host stand-in for <Arduino.h>; renderer.cpp is the real
# production renderer, compiled natively (it has no Arduino dependencies).
"$CXX" -std=gnu++11 -O1 -Wall -Istub \
    -o "$OUT/raid_engine_test" raid_engine_test.cpp ../src/renderer.cpp
"$OUT/raid_engine_test"
