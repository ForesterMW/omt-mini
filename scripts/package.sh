#!/usr/bin/env bash
# Produces the two release artifacts:
#   dist/OMTMini-<version>-win64.zip  portable, unzip and run
#   dist/OMTMini-Setup-<version>.exe  per user installer
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="${VERSION:-0.1.0}"
ARCH="${ARCH:-Winx64}"
RUNTIME="external/omt/bin/${ARCH}"

if [ ! -f "${RUNTIME}/libomt.dll" ]; then
    echo "Missing ${RUNTIME}/libomt.dll. Run scripts/fetch-omt.sh first." >&2
    exit 1
fi
if [ ! -f build-win/OMTMini.exe ]; then
    echo "Missing build-win/OMTMini.exe. Run scripts/cross-build.sh first." >&2
    exit 1
fi

STAGE="dist/stage"
rm -rf "${STAGE}" && mkdir -p "${STAGE}" dist

cp build-win/OMTMini.exe          "${STAGE}/"
cp build-win/omtmini_vcam.dll     "${STAGE}/"
cp "${RUNTIME}/libomt.dll"        "${STAGE}/"
cp "${RUNTIME}/libvmx.dll"        "${STAGE}/"
cp LICENSE                        "${STAGE}/LICENSE.txt"
cp external/omt/LICENSE-libomt.txt "${STAGE}/LICENSE-libomt.txt" 2>/dev/null || true

cat > "${STAGE}/README.txt" <<'TXT'
OMT Mini - portable build

Run OMTMini.exe. It lives in the system tray; right click the tray icon for
sources, desktop capture, the webcam output and settings.

Keep all four files together. OMTMini.exe loads libomt.dll and libvmx.dll from
its own folder.

The virtual camera needs to be registered before other applications can see it.
Open Settings, go to the Webcam tab, and press Register. That writes only to
HKEY_CURRENT_USER, so no administrator prompt appears.

Settings and logs: %APPDATA%\OMT Mini
TXT

ZIP="dist/OMTMini-${VERSION}-win64.zip"
rm -f "${ZIP}"
(cd "${STAGE}" && zip -q -9 -X "../../${ZIP}" ./*)

python3 scripts/pack_payload.py \
    build-win/OMTMini-Setup.exe \
    "dist/OMTMini-Setup-${VERSION}.exe" \
    "${STAGE}/OMTMini.exe" \
    "${STAGE}/omtmini_vcam.dll" \
    "${STAGE}/libomt.dll" \
    "${STAGE}/libvmx.dll" \
    "${STAGE}/LICENSE.txt" \
    "${STAGE}/README.txt"

rm -rf "${STAGE}"

echo
echo "Artifacts:"
ls -la dist/
( cd dist && sha256sum "OMTMini-${VERSION}-win64.zip" "OMTMini-Setup-${VERSION}.exe" \
    > "SHA256SUMS-${VERSION}.txt" && cat "SHA256SUMS-${VERSION}.txt" )
