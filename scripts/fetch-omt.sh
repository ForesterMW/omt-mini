#!/usr/bin/env bash
# Downloads the official Open Media Transport runtime DLLs.
#
# These are MIT licensed and published by the Open Media Transport project.
# They are deliberately not committed to this repository: it keeps the git
# history free of 7 MB binaries, and it means you always fetch them from the
# upstream release rather than trusting a copy here.
set -euo pipefail
cd "$(dirname "$0")/.."

OMT_VERSION="${OMT_VERSION:-v1.0.0.19}"
ARCH="${1:-Winx64}"   # Winx64 or Winarm64
DEST="external/omt/bin/${ARCH}"
URL="https://github.com/openmediatransport/libomtnet/releases/download/${OMT_VERSION}/OpenMediaTransport.Binaries.Release.${OMT_VERSION}.zip"

if [ -f "${DEST}/libomt.dll" ] && [ "${FORCE:-0}" != "1" ]; then
    echo "Already present: ${DEST}/libomt.dll (FORCE=1 to refetch)"
    exit 0
fi

mkdir -p "${DEST}" build-tmp
echo "Fetching Open Media Transport ${OMT_VERSION}..."
curl -fsSL -o build-tmp/omt.zip "${URL}"

unzip -o -q build-tmp/omt.zip "Libraries/${ARCH}/*" "LICENSE.txt" -d build-tmp/omt
cp "build-tmp/omt/Libraries/${ARCH}/libomt.dll" "${DEST}/"
cp "build-tmp/omt/Libraries/${ARCH}/libvmx.dll" "${DEST}/"
cp "build-tmp/omt/LICENSE.txt" "external/omt/LICENSE-libomt.txt"

# Keep the vendored header in step with the DLLs it describes.
cp "build-tmp/omt/Libraries/${ARCH}/libomt.h" external/omt/libomt.h

rm -rf build-tmp
echo "Runtime ready in ${DEST}:"
ls -la "${DEST}"
