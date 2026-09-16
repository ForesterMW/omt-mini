# OMT Mini

A single tray application for [Open Media Transport](https://openmediatransport.org),
the open, royalty free video over LAN protocol published by vMix and the Open
Media Transport contributors.

Find sources on your network, open as many live viewers as you want, share a
screen as a source, and appear as a webcam in other applications. All from one
tray icon, in a 798 KB executable that sits at effectively zero CPU when idle.

If you have used NDI Tools, this covers the same ground as Studio Monitor,
Screen Capture and Webcam Input, except it is one icon instead of several
programs, and it speaks OMT rather than NDI.

## Features

**Viewers.** Every source opens in its own window, so you can watch as many
feeds at once as your machine will carry.

- Colour conversion and scaling happen in a pixel shader, so a 1080p60 feed
  costs almost no CPU
- Audio monitoring with per channel dBFS meters, volume and mute
- Tally display: shows what the source reports across all of its receivers,
  as PGM and PVW lamps and a coloured strip along the top of the window. OMT
  Mini never asserts tally of its own. It is a monitor, and a monitor that
  told a source it was on air just because someone opened a window on it
  would be worse than useless
- Statistics panel with frame rate, bitrate, decode time and dropped frames
- Quality suggestion sent upstream to the sender
- Low bandwidth mode, which pulls the sender's one eighth preview stream
  instead of the full feed
- Frame and connection metadata, full screen, always on top

**Desktop capture.** Publishes a monitor as an OMT source using DXGI Desktop
Duplication, at 50 fps by default, with optional mouse cursor and system audio
via WASAPI loopback.

**Webcam output.** Receives an OMT source and presents it to other applications
as a normal camera named "OMT Mini Virtual Camera", so you can put a network
feed into Teams, Zoom, OBS or anything else that accepts a webcam.

**The source list** shows the address of every source beside its name, so the
thing you would have to type into another machine is never something you have
to go and look up. It tags what it finds. Anything advertised by the machine you
are sitting at is marked `this machine`, so it is obvious at a glance which
feeds are not crossing the network, and every source carries a tag saying what
it reports itself to be. Another OMT Mini shows up in green.

**Sources added by hand.** Senders that automatic discovery cannot see can be
addressed directly, the same idea as Access Manager in NDI Tools. They are
tagged `direct` and carry a green or red dot, because something added by hand
can be saved and then be switched off.

**One click updates.** Checks the public GitHub releases and installs the
latest stable build without leaving the window.

**Settings**, in tabs: General, Sources, Network, Viewer, Desktop, Webcam,
About. The Network tab drives libomt's own discovery server address and sender
port range.

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
appear within a second or two.

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

### Addresses

Every source in the list carries an address on its second line: the resolved IP
for a discovered source, or the `omt://host:port` for one added by hand. A
viewer shows the same thing in its statistics panel.

Desktop capture shows the address **other machines** should use to reach it,
next to the Start button, with a Copy button. libomt does not report which port
a sender bound to, so OMT Mini reads it back from the system rather than
guessing, which means the address shown is the real one.

### Adding a source by address

Discovery only reaches the local network. When a sender is somewhere it cannot
be seen, a different subnet, the far end of a VPN, or a host where mDNS is
blocked, press **+** in the source list, or add it under **Settings > Sources**.

Type an address and press Add:

| You type | OMT Mini uses |
|---|---|
| `10.0.0.5` | `omt://10.0.0.5:6400` |
| `10.0.0.5:6500` | `omt://10.0.0.5:6500` |
| `studio-pc.local` | `omt://studio-pc.local:6400` |
| `fe80::1` | `omt://[fe80::1]:6400` |

Port 6400 is assumed when you do not give one. Entries appear in the source
list and the tray menu beside discovered ones, and can be viewed or used as the
webcam source in exactly the same way.

To remove one, click it in the source list to select it and press Remove. Only
sources you added by hand can be removed: a discovered one is not yours to
delete, and would simply come back.

They are tagged `direct` and carry a status dot, checked every eight seconds
with a plain TCP connect, which costs far less than standing up a receiver to
find out. Green means the sender answered, red means it did not, grey means it
has not been checked yet. A red one can still be opened: the viewer sits at
"connecting" and picks the source up the moment it comes back. Discovered
sources are always green, since they would not be advertised otherwise.

Each entry points at one sender. To enumerate every source on a remote host
instead, run an
[OMT Discovery Server](https://github.com/openmediatransport/OMTDiscoveryServer)
and set its address under Settings > Network.

### What a source says it is

OMT senders describe themselves through `OMTSenderInfo`, a product name, a
manufacturer and a version. The source list shows that as a tag beside the
name, in grey for anything else and **green for another OMT Mini**.

That information only travels once a receiver is attached, so it cannot come
from discovery on its own. OMT Mini asks by opening a **metadata only**
receiver: no video or audio is requested, so it costs the sender one short
connection and nothing else, and the answer is remembered for as long as the
source stays on the network. It happens once per source, not on a timer.

Turn it off under Settings > General if you would rather nothing connected to a
source until you open it.

A viewer shows the same tag without any of that, because it is already
connected.

### Updates

**Settings > About** shows the running version, checks GitHub for the latest
stable release, and installs it in one press. The download is verified against
the `SHA256SUMS` file published with the release before it is run, and a
download that does not match is discarded.

The repository is public, so no account, token or sign in is involved.

Checking happens on launch by default and can be turned off. Installing is
always a separate, explicit press: nothing here restarts the application on its
own, which matters if the machine is on air.

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

About 10,300 lines of C++17 against raw Win32, Direct2D and Direct3D 11, with no
framework or package manager. The only runtime dependencies are Windows itself
and the two OMT DLLs.

| Area | What it does |
|---|---|
| `src/omt.*` | Binding to `libomt.dll`. Loaded with `LoadLibrary` rather than linked, so a missing runtime produces a readable message instead of a process that will not start |
| `src/gfx.*` | One shared D3D11 device. Each window gets a flip model swap chain with a Direct2D context bound to the same back buffer, so video and interface composite without an intermediate copy. UYVY, UYVA and BGRA convert in a pixel shader |
| `src/ui.*` | Immediate mode widgets drawn with Direct2D. Windows repaint on demand, so an idle window costs nothing |
| `src/window.*` | Window base class, DPI handling, input translation |
| `src/update.*` | Update check and install over WinHTTP, with SHA-256 verification through BCrypt |
| `src/discovery.*` | DNS-SD polling, hand added senders, and the reachability probe |
| `src/instance.h` | Finding and shutting down a running copy, shared with the installer |
| `src/netinfo.*` | Local address, host resolution, and reading back which port a sender actually bound to |
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
- A hand added source addresses one sender directly. Listing everything on a
  remote host needs a discovery server, because that is the only remote
  enumeration libomt offers.
- The reachability dot means a TCP connection was accepted on that port. It
  does not prove the thing listening is an OMT sender.
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
