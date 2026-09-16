# OMT Mini

A single tray application for [Open Media Transport](https://openmediatransport.org),
the open, royalty free video over LAN protocol published by vMix and the Open
Media Transport contributors.

Find sources on your network, open as many live viewers as you want, share a
screen as a source, and appear as a webcam in other applications. All from one
tray icon, in a 653 KB executable that sits at effectively zero CPU when idle.

If you have used NDI Tools, this covers the same ground as Studio Monitor,
Screen Capture and Webcam Input, except it is one icon instead of several
programs, and it speaks OMT rather than NDI.

## Features

**Viewers.** Every source opens in its own window, so you can watch as many
feeds at once as your machine will carry.

- Colour conversion and scaling happen in a pixel shader, so a 1080p60 feed
  costs almost no CPU
- Audio monitoring with per channel dBFS meters, volume and mute
- Tally in both directions: see what the sender reports, and send PGM or PVW
  back to it
- Statistics panel with frame rate, bitrate, decode time and dropped frames
- Quality suggestion sent upstream to the sender
- Low bandwidth mode, which pulls the sender's one eighth preview stream
  instead of the full feed
- Frame and connection metadata, full screen, always on top

**Desktop capture.** Publishes a monitor as an OMT source using DXGI Desktop
Duplication, with optional mouse cursor and system audio via WASAPI loopback.

**Webcam output.** Receives an OMT source and presents it to other applications
as a normal camera named "OMT Mini Virtual Camera", so you can put a network
feed into Teams, Zoom, OBS or anything else that accepts a webcam.

**Settings**, in tabs: General, Network, Viewer, Desktop, Webcam, About. The
Network tab drives libomt's own discovery server address and sender port range.

## Install

Download the latest [release](../../releases):

- **`OMTMini-Setup-<version>.exe`** installs to
  `%LOCALAPPDATA%\Programs\OMT Mini`. It writes nothing outside your user
  profile, so no administrator prompt appears, at install or at any point
  afterwards.
- **`OMTMini-<version>-win64.zip`** is the same files, portable. Unzip and run
  `OMTMini.exe`. Keep the folder intact: the executable loads `libomt.dll`
  and `libvmx.dll` from beside itself.

Requires Windows 10 or 11, 64 bit. Settings and logs are kept in
`%APPDATA%\OMT Mini`.

To remove it, use Apps & features, or run `OMTMini.exe --uninstall`.

`SHA256SUMS-<version>.txt` in each release covers both downloads.

## Using it

Left click the tray icon for the source list. Right click it for a menu:
sources, desktop capture, the webcam output and settings.

Sources on the local network are discovered automatically over DNS-SD and
appear within a second or two. If your sources are on another subnet, set a
discovery server under Settings > Network.

Keyboard shortcuts in a viewer window:

| Key | Action |
|---|---|
| `F` or `F11` | Full screen |
| `Esc` | Leave full screen |
| `M` | Mute audio |
| `S` | Statistics panel |
| `D` | Metadata panel |
| `T` | Always on top |
| Double click | Full screen |

### Firewall

OMT Mini's own senders bind to TCP ports 6400 to 6600 by default, which you can
change under Settings > Network. Allow that range, and allow mDNS on UDP 5353
for discovery, if your firewall does not already.

### The virtual camera

Other applications only see the camera once its DirectShow filter has been
registered. The installer offers to do this, or you can do it later from
Settings > Webcam > Register.

Registration writes to `HKEY_CURRENT_USER` only, which is why it needs no
elevation, and it is undone by Unregister or by uninstalling.

## Build

The repository builds without any downloads: the `libomt.h` header it compiles
against is vendored in `external/omt`, and `libomt.dll` is loaded at runtime
rather than linked.

Cross compiling from Linux with mingw-w64 is the tested path, and is how the
releases are built:

```
sudo apt install mingw-w64 cmake zip
./scripts/cross-build.sh
```

Building on Windows with MSVC and CMake should also work, though it is not
exercised by the releases:

```
cmake -B build
cmake --build build --config Release
```

To run what you built, put `libomt.dll` and `libvmx.dll` next to `OMTMini.exe`.
`./scripts/fetch-omt.sh` downloads them from the official
[libomtnet release](https://github.com/openmediatransport/libomtnet/releases)
into `external/omt/bin/`, or you can take them from a release archive. They are
not committed here, so this repository stays free of binaries you would
otherwise have to take on trust.

To produce the release artifacts, a portable zip and the self extracting
installer, in `dist/`:

```
./scripts/fetch-omt.sh
./scripts/cross-build.sh
./scripts/package.sh
```

The shell scripts need bash. On Windows, run them under Git Bash or WSL, or
follow what they do by hand.

## How it is put together

About 8,700 lines of C++17 against raw Win32, Direct2D and Direct3D 11, with no
framework or package manager. The only runtime dependencies are Windows itself
and the two OMT DLLs.

| Area | What it does |
|---|---|
| `src/omt.*` | Binding to `libomt.dll`. Loaded with `LoadLibrary` rather than linked, so a missing runtime produces a readable message instead of a process that will not start |
| `src/gfx.*` | One shared D3D11 device. Each window gets a flip model swap chain with a Direct2D context bound to the same back buffer, so video and interface composite without an intermediate copy. UYVY, UYVA and BGRA convert in a pixel shader |
| `src/ui.*` | Immediate mode widgets drawn with Direct2D. Windows repaint on demand, so an idle window costs nothing |
| `src/window.*` | Window base class, DPI handling, input translation |
| `src/viewer.*` | A viewer window and its receiver thread |
| `src/capture.*` | Desktop Duplication into an OMT sender, on its own device and thread |
| `src/webcam.*` | Receiver writing into a shared memory ring that the filter reads |
| `vcam/` | The DirectShow source filter. Written against the raw COM interfaces because the DirectShow base classes are an MSVC sample that a mingw cross build cannot use |
| `installer/` | Self extracting per user installer, payload appended to the executable |

## Known limits

- **The virtual camera is the least proven part of this.** A DirectShow filter
  is loaded into other applications' processes and behaviour varies between
  hosts, so treat it as beta. If a particular application does not see the
  camera or misbehaves with it, please
  [open an issue](../../issues) and say which application and which Windows
  version.
- 64 bit host applications only. A 32 bit application looking for a camera will
  not find it yet.
- P216 and PA16 high bit depth formats are received by libomt but not yet
  rendered, so they are not offered in the format list.
- Desktop capture sends at the monitor's native resolution. There is no
  downscale option yet.
- Windows only. The protocol is cross platform and so is most of the logic, but
  the interface, capture and camera layers are Win32.

## Contributing

Issues and pull requests are welcome, particularly reports of host applications
that do not get on with the virtual camera. There is no contributor licence
agreement and no style config to install; match the surrounding code.

## Credits and licence

OMT Mini is MIT licensed. See [LICENSE](LICENSE).

It uses `libomt` and `libvmx` from the Open Media Transport project, which are
MIT licensed and redistributed unmodified in the release archives. Their licence
is included as `LICENSE-libomt.txt`.

This project is not affiliated with, endorsed by, or supported by vMix
(StudioCoast Pty Ltd) or the Open Media Transport project.
