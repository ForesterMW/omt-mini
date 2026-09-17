# OMT Mini

**Every Open Media Transport tool you need on Windows, in one tray icon.**
Viewer, multiviewer, screen capture, virtual webcam and source management, in a
1 MB executable that idles at effectively zero CPU.

[![Latest release](https://img.shields.io/github/v/release/ForesterMW/omt-mini?label=download&color=3b82f6)](../../releases/latest)
[![Downloads](https://img.shields.io/github/downloads/ForesterMW/omt-mini/total?color=22c55e)](../../releases)
[![License](https://img.shields.io/badge/licence-MIT-informational)](LICENSE)
![Windows 10 and 11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078d4)

[Open Media Transport](https://openmediatransport.org) is the open, royalty
free video over LAN protocol published by vMix and the OMT contributors, and an
alternative to NDI that costs nothing to implement. OMT Mini is a complete set
of everyday tools for it, as one small program rather than several.

## What it replaces

If you know NDI Tools, you already know what this is.

| NDI Tools | OMT Mini | |
|---|---|---|
| Studio Monitor | **Viewer** | As many at once as your machine will carry |
| Screen Capture | **Desktop capture** | Any monitor, cursor and system audio |
| Webcam Input | **Virtual webcam** | Appears as a camera in Teams, Zoom, OBS |
| Access Manager | **Direct sources** | Reach a sender on another subnet, and put it on mDNS for everyone else |
| Studio Monitor, several of them | **Multiview** | One wall, seven layouts, and it can be sent as a source of its own |
| nothing like it | **Web control panel** | Drive the whole thing from a browser on another machine |

All of it from one tray icon, one install, one thing to update.

## Features

### Viewers

Every source opens in its own window. Open as many as you like.

- Colour conversion and scaling run in a pixel shader, so a 1080p60 feed costs
  almost no CPU
- Audio monitoring with per channel dBFS meters, volume and mute
- Tally display, red for program and green for preview, as lamps and a strip
  along the top of the window
- Statistics panel: frame rate, bitrate, decode time, dropped frames, the
  sender's own product name and the address it is on
- Quality suggestion sent upstream, and a low bandwidth mode that pulls the
  sender's one eighth preview stream instead of the full feed
- Full screen, always on top, frame and connection metadata

### Multiview

One window showing many sources at once, in `2x2`, `3x2`, `3x3`, `4x3`, `4x4`,
or the classic hero shapes `1 + 5` and `1 + 7`.

- Click any cell to choose what goes in it
- Tally borders the whole cell
- Full screen on `F11` or a double click, and it can open at startup straight
  into full screen
- Tiles use OMT's **preview mode** by default, the sender's own one eighth
  stream, so sixteen tiles cost a fraction of sixteen full feeds
- When the pointer stops moving the controls and labels fade away and you are
  left with nothing but pictures

**The wall can be sent to the network as a single OMT source.** One machine
builds it, every other machine just receives it, and it does not need the
window open to do that, so a spare box can compose a wall headless and hand it
to the gallery.

### Desktop capture

Publishes a monitor as an OMT source using DXGI Desktop Duplication, at 50 fps
by default, with optional mouse cursor and system audio via WASAPI loopback. It
shows the address other machines should use to reach it, with a Copy button.

### Virtual webcam

Receives an OMT source and presents it to other applications as a normal camera
called "OMT Mini Virtual Camera", so a network feed can go into Teams, Zoom,
OBS or anything else that takes a webcam. Registers under `HKEY_CURRENT_USER`,
so no administrator prompt.

### Sources, found and added

Sources on the local network appear automatically. The list tells you what each
one is: `this machine` for your own, `direct` for one you added, the product it
reports itself to be, green for another OMT Mini, and a green or red dot for
whether an added source is answering.

For a sender this network cannot see, on another subnet, at the far end of a
VPN, or on a host with mDNS blocked, add it by address. **Leave the port off
and OMT Mini finds every sender on that machine**, walking the port range and
confirming each one is really an OMT sender before saving it.

Added sources can then be **announced on mDNS**, which makes them visible to
vMix and everything else that browses for OMT sources, with no video passing
through this machine.

### Web control panel

A page served by OMT Mini itself, for driving it from a browser on another
machine. Off by default, turned on under **Settings > Web**.

- Every open viewer and the multiview drawn **where they actually are**, to
  scale, across your real monitor layout
- Drag a window to move it, the corner to resize, double click to maximise,
  and the window on the machine follows
- **Drag a source from the list onto a window to retarget it**, or onto empty
  desktop to open a new viewer
- Drag sources onto the multiview cells, and change its layout
- Add a source by address remotely, including the port walk

Nothing to install at the other end and no dependencies: it is one page served
from memory.

It is built to stay out of the way. The whole server is a single non-blocking
loop, so there are no per connection threads to leak or fail to create;
connections, request sizes and the command queue are all capped; and every
connection has a deadline, so a client that stalls cannot pile up. Nothing a
request touches belongs to the interface: the page and the state are strings
prepared on the interface thread, and commands are queued for it to apply, so a
browser can never reach a window directly.

### Kept up to date

Checks the public GitHub releases and installs the latest stable build in one
press, verified against its published checksum. Checking and installing are
deliberately separate, because a machine that is on air should never restart
itself.

## Install

Download the latest [release](../../releases/latest):

- **`OMTMini-Setup-<version>.exe`** (8.7 MB) installs to
  `%LOCALAPPDATA%\Programs\OMT Mini`. It writes nothing outside your user
  profile, so **no administrator prompt**, at install or ever after.
- **`OMTMini-<version>-win64.zip`** (3.8 MB) is the same files, portable.
  Unzip and run `OMTMini.exe`.

Windows 10 or 11, 64 bit. Settings and logs live in `%APPDATA%\OMT Mini`.
To remove it, use Apps & features, or run `OMTMini.exe --uninstall`.

Most of the download is the official `libomt.dll` and `libvmx.dll` from the
Open Media Transport project, redistributed unmodified. OMT Mini itself is
1 MB.

## Using it

Left click the tray icon for the source list. Right click it for a menu:
sources, multiview, desktop capture, the webcam output and settings.

Keyboard, in a viewer or the multiview:

| Key | Action |
|---|---|
| `F` or `F11` | Full screen |
| `Esc` | Leave full screen |
| `M` | Mute audio |
| `S` | Statistics panel |
| `D` | Metadata panel |
| `T` | Always on top |
| Double click | Full screen |

### Adding a source by address

Press **+** in the source list, or use **Settings > Sources**.

| You type | OMT Mini uses |
|---|---|
| `10.0.0.5` | walks the machine and finds every sender on it |
| `10.0.0.5:6500` | `omt://10.0.0.5:6500`, exactly as given |
| `studio-pc.local` | walks that host |
| `fe80::1` | `omt://[fe80::1]:6400` |

Leaving the port off walks upward from 6400, gives up after ten ports with
nothing on them, and resets that budget every time it finds something, so a
machine with senders spread across the range is still covered. Each hit is
confirmed as a real OMT sender, not merely an open port, and named after what
it reports itself to be.

Click an entry to select it and it offers **Edit** and **Remove**. Only sources
you added by hand can be changed: a discovered one is not yours to delete, and
would come straight back.

### Announcing added sources to the rest of the network

A source you added by address is one this network could not see. vMix and every
other OMT tool find sources by browsing mDNS, so **Settings > Sources >
Announce direct sources over mDNS** puts them there. They then appear in those
applications by name, like any other source, with nobody typing an address.

On by default. Turn it off and each entry gets its own Announce button.

**No video passes through this machine.** Each announcement is a sender that
carries nothing and redirects to the machine the source actually lives on,
which is what OMT calls a virtual source. Whatever connects is sent straight to
the original host for the pictures, so this machine is not in the media path
and adds no latency or bandwidth.

Only added sources are announced. Anything already discovered is already on
mDNS, and a machine that picks up one of these announcements sees it as a
discovered source rather than an added one, so it will not announce it onward.

A source added without a name is called `Direct source` and can be renamed. It
is deliberately not named after its address: a dot ends a label in DNS-SD and
libomt does not escape them, so anything named after an IP is not announced at
all.

### Firewall

OMT Mini's senders bind to TCP 6400 to 6600 by default, which you can change
under Settings > Network. Allow that range, and mDNS on UDP 5353 for discovery.

### The virtual camera

Other applications only see the camera once its DirectShow filter is
registered. The installer offers to do it, or use Settings > Webcam > Register.
Registration writes to `HKEY_CURRENT_USER` only, which is why it needs no
elevation, and Unregister or uninstalling undoes it.

## Build

The repository builds with no downloads: the `libomt.h` it compiles against is
vendored, and `libomt.dll` is loaded at runtime rather than linked.

Cross compiling from Linux with mingw-w64 is the tested path, and is how the
releases are built:

```
sudo apt install mingw-w64 cmake zip
./scripts/cross-build.sh
```

On Windows with MSVC and CMake, which is not exercised by the releases:

```
cmake -B build
cmake --build build --config Release
```

To run it, put `libomt.dll` and `libvmx.dll` next to `OMTMini.exe`.
`./scripts/fetch-omt.sh` downloads them from the official
[libomtnet release](https://github.com/openmediatransport/libomtnet/releases),
or take them from a release archive. They are not committed here, so this
repository stays free of binaries you would have to take on trust.

Release artifacts, the portable zip and the installer, in `dist/`:

```
./scripts/fetch-omt.sh && ./scripts/cross-build.sh && ./scripts/package.sh
```

The shell scripts need bash. On Windows use Git Bash or WSL.

## How it is put together

About 14,700 lines of C++17 against raw Win32, Direct2D and Direct3D 11, with
no framework and no package manager. The only runtime dependencies are Windows
itself and the two OMT DLLs.

| Area | What it does |
|---|---|
| `src/omt.*` | Binding to `libomt.dll`, loaded with `LoadLibrary` so a missing runtime says so rather than failing to start |
| `src/gfx.*` | One shared D3D11 device. Each window gets a flip model swap chain with a Direct2D context on the same back buffer, so video and interface composite without an intermediate copy. UYVY, UYVA and BGRA convert in a pixel shader |
| `src/ui.*` | Immediate mode widgets on Direct2D. Windows repaint on demand, so an idle window costs nothing |
| `src/window.*` | Window base, DPI, input |
| `src/viewer.*` | A viewer and its receiver thread |
| `src/multiview.*` | The multiview engine, its window, and the output that composes it into an OMT sender |
| `src/discovery.*` | DNS-SD polling, added senders, reachability, identification |
| `src/announce.*` | Putting added sources on mDNS as redirecting virtual sources |
| `src/scan.*` | Walking a host's port range to find every sender on it |
| `src/capture.*` | Desktop Duplication into an OMT sender, on its own device and thread |
| `src/webcam.*` | Receiver writing into a shared memory ring the filter reads |
| `src/update.*` | Update check and install over WinHTTP, verified with BCrypt |
| `src/netinfo.*` | Local address, host resolution, and reading back which port a sender took |
| `src/webui.*` | The control panel's HTTP server and its API |
| `src/webpage.h` | The control panel itself, one page with no dependencies |
| `vcam/` | The DirectShow source filter, written against raw COM because the DirectShow base classes are an MSVC sample a mingw cross build cannot use |
| `installer/` | Self extracting per user installer, payload appended to the executable |

## Known limits

- **The virtual camera is the least proven part.** A DirectShow filter is
  loaded into other applications' processes and behaviour varies between hosts,
  so treat it as beta. If an application does not see the camera, please
  [open an issue](../../issues) saying which application and which Windows
  version.
- 64 bit host applications only for the camera. A 32 bit one will not find it.
- P216 and PA16 high bit depth formats are received by libomt but not yet
  rendered, so they are not offered.
- Desktop capture sends at the monitor's native resolution.
- Multiview has no audio, on screen or sent. It is a wall, not a monitoring
  position.
- An added source addresses one sender. Listing everything on a remote host
  needs a discovery server, which is the only remote enumeration libomt offers.
- The reachability dot means a TCP connection was accepted, not that an OMT
  sender is behind it. The port walk goes further and confirms each hit.
- **The control panel has no password.** Anyone who can reach its port can
  move, retarget and close windows on that machine. It is off by default and
  should only be turned on where you trust the network.
- Windows only. The protocol is cross platform and so is most of the logic, but
  the interface, capture and camera layers are Win32.

## Contributing

Issues and pull requests are welcome, particularly reports of applications that
do not get on with the virtual camera. No contributor licence agreement, no
style config to install; match the surrounding code.

## Credits and licence

MIT licensed, see [LICENSE](LICENSE).

Uses `libomt` and `libvmx` from the Open Media Transport project, MIT licensed
and redistributed unmodified in the release archives. Their licence is included
as `LICENSE-libomt.txt`.

Not affiliated with, endorsed by, or supported by vMix (StudioCoast Pty Ltd) or
the Open Media Transport project.
