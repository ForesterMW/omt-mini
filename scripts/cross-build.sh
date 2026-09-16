#!/usr/bin/env bash
# Cross compile OMT Mini for Windows x64 from Linux with mingw-w64.
#
# Requires: x86_64-w64-mingw32-g++, cmake, and the OMT runtime
# (run scripts/fetch-omt.sh first).
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${VERSION:-0.1.0}"

cmake -B build-win \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
    -DCMAKE_BUILD_TYPE=Release \
    -DOMTMINI_VERSION="${VERSION}" \
    "$@"

cmake --build build-win -j"$(nproc)"

x86_64-w64-mingw32-strip build-win/OMTMini.exe \
                         build-win/omtmini_vcam.dll \
                         build-win/OMTMini-Setup.exe

ls -la build-win/OMTMini.exe build-win/omtmini_vcam.dll build-win/OMTMini-Setup.exe
