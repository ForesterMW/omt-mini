# OMT Mini

One small tray application for [Open Media Transport](https://openmediatransport.org):
find sources on the network, open as many live viewers as you want, share a
screen, and appear as a webcam in other apps.

OMT is the open, royalty free video over LAN protocol published by vMix and the
Open Media Transport contributors. The official Windows tooling ships as a set
of separate executables. OMT Mini is the same jobs in a single tray icon, in a
668 KB binary that idles at effectively zero CPU.

## What it does

**Viewers.** Right click the tray icon and pick a source, or open the source
list and press View. Every viewer is its own window, so you can watch as many
feeds at once as your machine will carry. Each one gives you:

- GPU colour conversion and scaling, so a 1080p60 feed costs almost no CPU
- audio monitoring with per channel dBFS meters, volume and mute
- tally, both directions: see what the sender reports, and send PGM/PVW back
- a statistics panel with frame rate, bitrate, decode time and dropped frames
- quality suggestion sent upstream, plus a low bandwidth mode that pulls the
  sender's one eighth preview stream instead of the full feed
- frame and connection metadata, full screen, always on top

**Desktop capture.** Publishes a monitor as an OMT source using DXGI Desktop
Duplication, with optional cursor and system audio via WASAPI loopback.

**Webcam output.** Receives an OMT source and presents it to other applications
as a normal camera called "OMT Mini Virtual Camera", so you can put a feed into
Teams, Zoom, OBS or anything else that takes a webcam.

**Settings**, tabbed: General, Network, Viewer, Desktop, Webcam, About. The
network tab drives libomt's own discovery server and sender port range.

## Install

Grab the latest [release](../../releases):

- `OMTMini-Setup-<version>.exe` installs to `%LOCALAPPDATA%\Programs\OMT Mini`.
  It never writes outside your user profile, so there is no administrator
  prompt, at install or at any point afterwards.
- `OMTMini-<version>-win64.zip` is the same files, portable. Unzip and run.

Windows 10 or 11, 64 bit. Settings and logs live in `%APPDATA%\OMT Mini`.

To uninstall, use Apps & features, or run `OMTMini.exe --uninstall`.

### The virtual camera

Other applications only see the camera once the DirectShow filter is
registered. The installer offers to do it, or you can do it later from
Settings > Webcam > Register. Registration writes to `HKEY_CURRENT_USER` only,
which is why it needs no elevation, and it is reversed by Unregister or by
uninstalling.

## Build

Windows, with MSVC:

```
scripts\fetch-omt.sh        # or download the runtime by hand, see below
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Cross compiling from Linux with mingw-w64, which is how the releases are made:

```
sudo apt install mingw-w64 cmake
./scripts/fetch-omt.sh      # downloads libomt.dll and libvmx.dll
./scripts/cross-build.sh
./scripts/package.sh        # writes dist/
```

`scripts/fetch-omt.sh` pulls the official
[libomtnet release](https://github.com/openmediatransport/libomtnet/releases)
into `external/omt/bin/`. Those DLLs are not committed here, so the repository
stays free of binaries you would otherwise have to take on trust.

## How it is put together

| | |
|---|---|
| `src/omt.*` | Runtime binding to `libomt.dll`, loaded with `LoadLibrary` so a missing runtime gives a readable error instead of a failed process start |
| `src/gfx.*` | One shared D3D11 device; per window flip model swap chain with a Direct2D context on the same back buffer. UYVY, UYVA and BGRA are converted in a pixel shader |
| `src/ui.*` | Immediate mode widgets on Direct2D. Windows repaint on demand, so an idle window costs nothing |
| `src/viewer.*` | A viewer window and its receiver thread |
| `src/capture.*` | Desktop Duplication into an OMT sender, on its own device and thread |
| `src/webcam.*` | Receiver into a shared memory ring the filter reads |
| `vcam/` | The DirectShow source filter, written against raw COM because the DirectShow base classes are an MSVC sample and are not available to a mingw cross build |
| `installer/` | Self extracting per user installer, payload appended to the exe |

Roughly 6,000 lines of C++17. The only runtime dependencies are Windows itself
and the two OMT DLLs.

## Known limits

- **The virtual camera is the least proven part.** It has been built and its
  structure reviewed, but a DirectShow filter is loaded into other people's
  processes and behaviour varies between host applications. Treat it as beta
  and report what breaks.
- 64 bit host applications only. A 32 bit app looking for a camera will not
  see it yet.
- P216 and PA16 high bit depth formats are received by libomt but not yet
  rendered, so they are not offered in the format list.
- Desktop capture sends at the monitor's native resolution; there is no
  downscale option yet.
- Windows only. The protocol is cross platform and so is most of the logic, but
  the UI, capture and camera layers are Win32.

## Credits and licence

OMT Mini is MIT licensed. See [LICENSE](LICENSE).

It links `libomt` and `libvmx` from the Open Media Transport project, which are
MIT licensed and redistributed unmodified in the release archives. Their licence
is included as `LICENSE-libomt.txt`.

This project is not affiliated with, endorsed by, or supported by vMix
(StudioCoast Pty Ltd) or the Open Media Transport project.
