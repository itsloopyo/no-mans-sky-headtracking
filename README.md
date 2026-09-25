# No Man's Sky Head Tracking

![No Man's Sky running with this mod](https://raw.githubusercontent.com/itsloopyo/no-mans-sky-headtracking/main/assets/readme-clip.gif)

An unofficial head tracking mod for No Man's Sky that moves the view with your
head while your mouse or controller keeps aiming, driven by a webcam, phone, or
any OpenTrack compatible tracker, with no VR headset required.

> **Updating from 0.1.0?** `HeadTracking.ini` has a new layout. The first time
> this version starts, it converts your file once and keeps the old one beside
> it as `HeadTracking.ini.pre-canonical`. The sensitivity, axis inversion and
> `[Reticle] FollowAim` settings are gone: set sensitivity and inversion in your
> tracker, and the crosshair now always follows your aim. See
> [Configuration](#configuration).

## Features

- **Decoupled look and aim** - head tracking moves the view; aim stays on your mouse or controller
- **6DOF positional tracking** - lean and peek with head position
- **Works with any OpenTrack compatible tracker** - free options available for PC, iOS and Android

## Requirements

- [No Man's Sky](https://store.steampowered.com/app/275850/No_Mans_Sky/),
  64-bit `Binaries\NMS.exe` on a compatible store:
  - **Steam** 
  - **Xbox Game Pass / Microsoft Store**
- A tracking source: [OpenTrack](https://github.com/opentrack/opentrack/releases)
  with a webcam or other supported device, or a phone app that sends the
  OpenTrack UDP protocol itself.
- Windows 10 or 11, 64-bit.

## Installation

### Lopari

Download [Lopari](https://lopari.app), choose **No Man's Sky**, and click
**Play with head tracking**.

### Standalone Installer

1. Download the latest installer ZIP from the
   [Releases page](https://github.com/itsloopyo/no-mans-sky-headtracking/releases).
2. Extract it anywhere.
3. Double-click `install.cmd`. It auto-detects No Man's Sky on Steam, GOG and
   Xbox Game Pass. If you own it on more than one store, it installs into the
   copy it finds first - run it again with that copy's folder as an argument to
   cover the other one.
4. Configure OpenTrack to output UDP to `127.0.0.1:4242` (see
   [Setting Up OpenTrack](#setting-up-opentrack)).
5. Launch the game.

If the installer cannot find your copy of the game, point it at the install
folder yourself, either with an environment variable:

```powershell
$env:NO_MANS_SKY_PATH = "D:\Games\No Man's Sky"
```

or as a positional argument:

```powershell
install.cmd "D:\Games\No Man's Sky"
```

For an Xbox Game Pass copy the folder to point it at is the one holding
`Binaries`, which is the `Content` folder rather than the one named after the
game:

```powershell
install.cmd "D:\XboxGames\No Man's Sky\Content"
```

### Manual Installation

The `-nexus.zip` from the same release holds `XINPUT9_1_0.dll` plus
`README.md`, `LICENSE` and `THIRD-PARTY-NOTICES.md`. Copy `XINPUT9_1_0.dll` into
the `Binaries` folder next to `NMS.exe`. The mod creates `HeadTracking.ini`
beside it the first time the game starts, and an existing one is kept. There is
no separate mod loader to install: No Man's Sky imports `XINPUT9_1_0.dll` and
Windows searches the game folder before `System32`, so the game loads the shim
on startup and the shim forwards
`XInputGetState` and `XInputSetState` on to the genuine copy in `System32`,
leaving controller input untouched. To remove a manual install, delete
`XINPUT9_1_0.dll`, and `HeadTracking.ini` too if you do not want to keep your
settings.

## Setting Up OpenTrack

The mod listens for OpenTrack pose data on UDP port `4242`, on every network
interface. One datagram is six little-endian 64-bit floats in the order
`x, y, z, yaw, pitch, roll`: position in centimeters, rotation in degrees, 48
bytes. Anything longer is accepted too, with the first 48 read. Anything that sends that to that port drives the view.
OpenTrack's **UDP over network** output sends exactly this, and the steps below
set it up.

1. Install [OpenTrack](https://github.com/opentrack/opentrack/releases).
2. Pick a tracker under **Input**, using the notes below.
3. Set **Output** to **UDP over network**, host `127.0.0.1`, port `4242`.
4. Press **Start**. Tracking and the game can start in either order.

### VR Headset Setup

1. Connect the headset over Air Link, Virtual Desktop, or a link cable.
2. Start SteamVR.
3. Set OpenTrack's **Input** to the SteamVR tracker.
4. Leave **Output** on **UDP over network**, host `127.0.0.1`, port `4242`.

### Webcam Setup

OpenTrack ships a `neuralnet tracker` input that reads a plain webcam, with no
markers and no IR hardware. Select it under **Input**, pick your camera in its
settings, and use the output settings above. How well it tracks depends on your
camera and your lighting, so try it before buying anything.

### Phone App Setup

A phone app can reach the mod directly, with no OpenTrack on the PC, if it
sends the datagram described above. Point it at this PC's IP address (run
`ipconfig` to find it) on port `4242`. Not every phone tracker speaks this
protocol, so check yours for an OpenTrack or UDP output option first.

What decides direct send versus a relay is how much filtering the app does
before the packet leaves the phone. The mod's smoothing is sized to take the
edge off a clean signal rather than to rescue a noisy one, so a raw or lightly
filtered feed sent direct will jitter. The test is quick: send direct, hold your
head still, and if the view drifts or shakes, route it through OpenTrack
instead. Point the app at OpenTrack's **UDP over network** *input* on some other
port, say 5252, and let OpenTrack's filters and curves clean the feed up before
its output forwards to `127.0.0.1:4242`.

I made [Headcam](https://headcam.app) so decent tracking was free for anybody
with a phone already in their pocket. It filters on-device, so it can send
direct. Any app that filters enough noise works the same way.

Anything arriving from outside `127.0.0.0/8` counts as a remote connection and
is smoothed with `RemoteSmoothing` rather than `LocalSmoothing`. That includes a
tracker on this very PC that sends to the machine's own LAN address, because the
mod reads the source address and not the machine.

### Centering

Centering belongs to your tracker. The mod applies the pose it receives exactly
as it arrives, so a stream of zeros holds the view where the game itself puts
it. Press the center control in your tracker and it zeroes its own output:
OpenTrack has a **Center** bind, SteamVR has its own reset, and Headcam has a
CENTER button.

## Controls

Yaw automatically follows the walking horizon on foot and uses the camera's
local axis in a ship or during a spacewalk.

Each action has a nav-cluster key and a chord by default, use whichever your
keyboard has. Both are set in `[Hotkeys]` in `HeadTracking.ini`, where you can
change or remove either.

| Action              | Nav-cluster | Chord           |
|---------------------|-------------|-----------------|
| Toggle tracking     | `End`       | `Ctrl+Shift+Y`  |
| Cycle tracking mode | `Page Up`   | `Ctrl+Shift+G`  |

`Page Up` / `Ctrl+Shift+G` cycles tracking mode:

1. Normal head-tracked gameplay
2. Positional tracking disabled, rotational tracking enabled
3. Rotational tracking disabled, positional tracking enabled
4. Back to normal

The mode you pick is saved to `HeadTracking.ini` and is the mode the next launch
starts in. `End` turns tracking on and off for the current session only; whether
it is on at startup is `[General] EnableOnStartup`.

### Aiming down sights

Head tracking stays on while you aim. The weapon stays where your mouse or
controller points it, so with your head turned it sits off to one side with its
sights still lined up, and your rounds land where those sights point.

## Configuration

<!-- cameraunlock:config -->
The mod reads its settings from `Binaries\HeadTracking.ini` in the game folder, and creates the file when it starts and finds none. Edit it with any text editor.

Earlier versions of the mod used an older layout for this file. The first time this version starts, it converts the file once into the layout below and keeps the file as it was beside it as `HeadTracking.ini.pre-canonical`. `HeadTracking.ini.pre-canonical.last`, when present, is the file as it was before the most recent conversion: the mod converts the file again when it finds the older layout later, for example after an older version of the mod rewrote it.

Comments, and keys the mod never read, are not carried over. Nor are these, where your old file had them:

- Reticle settings, and a key that toggled the reticle.
- A sensitivity, scale, deadzone, response curve or axis inversion you changed from its default. Set these in your tracker instead.
- The setting for a feature that earlier versions shipped switched off while it was untested. It now follows the mod's default.

An older version of the mod may not read the new layout correctly. It reads a key that moved as its own default, and it can misread a hotkey or another value that is now written as a name. To go back to an older version, first copy `HeadTracking.ini.pre-canonical` back over `HeadTracking.ini`, which restores the old file.

With every setting at its default, the file reads:

```ini
; No Man's Sky head tracking settings.
; Comments start with ; and go on their own line. Text after a value is part of the value.
; Hotkeys are key names such as End, PageUp or Ctrl+Shift+Y. Separate several with commas; leave empty for none.

[CameraUnlock]
; Written by the mod. Leave this section in place.
ConfigFormat=1

[Network]
; UDP port the mod receives tracker data on (OpenTrack protocol).
UdpPort=4242

[General]
; true: head tracking is on when the game starts. ToggleKey turns it on and off.
EnableOnStartup=true
; true: turning your head turns the view.
; Tracking mode at startup, with PositionEnabled. The mode hotkey changes both.
RotationEnabled=true

[Smoothing]
; Smoothing when the tracker runs on this PC. 0 is the least, 1 the most.
LocalSmoothing=0.0
; Smoothing when the tracker is another device on the network, such as a phone.
; 0 is the least, 1 the most.
RemoteSmoothing=0.15

[Position]
; true: moving your head moves the view.
; Tracking mode at startup, with RotationEnabled. The mode hotkey changes both.
PositionEnabled=true
; How far, in metres, leaning left or right can move the view.
PositionLimitX=0.3
; How far, in metres, raising your head can move the view.
PositionLimitY=0.2
; How far, in metres, lowering your head can move the view.
PositionLimitYDown=0.2
; How far, in metres, leaning forward can move the view.
PositionLimitZ=0.4
; How far, in metres, leaning back can move the view.
PositionLimitZBack=0.1

[Hotkeys]
; Turns head tracking on and off.
ToggleKey=End, Ctrl+Shift+Y
; Changes the tracking mode: rotation and position, rotation only, position only.
CycleTrackingModeKey=PageUp, Ctrl+Shift+G

[Logging]
; true: write HeadTracking.log beside NMS.exe. It starts fresh every launch.
WriteLog=true

[Debug]
; Everything in this section is for tracking down a fault; leave it as it is unless you
; were asked to change it.
; true: write engine-discovery detail to HeadTracking.log. Turn it on for a bug report.
Diagnostics=false
; true: log which instructions read the live camera transform, using a CPU data
; breakpoint. The game crawls while each burst is armed. ReadWatch and WriteWatch share
; one debug register, so only the first of the two to start is armed.
ReadWatch=false
; true: log which instructions write the live camera transform, the same way.
WriteWatch=false
; true: serve the clean camera to one candidate caller at a time, cycling every few
; seconds, and log each one, to find which consumer aims something that aims wrongly.
AimCallerSweep=false
; true: the same sweep the other way round, serving the tracked camera to one candidate
; at a time, to find what decides visibility.
CullCallerSweep=false
; Stops AimCallerSweep or CullCallerSweep on the candidate it is serving and logs it.
SweepFreezeKey=Ctrl+Shift+J
; true: count every caller of the camera accessor and write the counts to the log
; every five seconds.
CallerCensus=false
; true: use AimTransformCallers, AimCopyCallers and TrackedTransformCallers in place of
; the build's own callers, and read them again whenever this file changes.
LiveCallerOverrides=false
; true: locate the first-person weapon transform and write one report to the log.
WeaponProbe=false
; true: hold each engine camera global clean in turn, to find which one places the
; first-person weapon.
CleanGlobalCycle=false
; true: hunt for the crosshair's screen position in memory.
CrosshairProbe=false
; true: walk the crosshair around a fixed square instead of following the aim, to tell
; a crosshair that does not move apart from one that moves to the wrong place.
ReticleSweep=false
; Caller addresses, relative to NMS.exe, for LiveCallerOverrides: those served the clean
; camera through the accessor. Empty keeps the build's own.
; AimTransformCallers=
; Those served the clean camera through the copy. Empty keeps the build's own.
; AimCopyCallers=
; Those served the tracked camera through the accessor. Empty keeps the build's own.
; TrackedTransformCallers=
; Byte offset into the camera transform that ReadWatch covers.
; ReadWatchOffset=0x0
; Address, relative to NMS.exe, of the once-a-frame scene sample in place of the build's
; own. Empty keeps the build's own; 0x0 turns the sample off.
; SceneSampleRva=
```
<!-- /cameraunlock:config -->

## Troubleshooting

- **Mod not loading.** Check that `HeadTracking.log` exists in `Binaries`. If it
  does not, verify `XINPUT9_1_0.dll` sits in `Binaries` next to `NMS.exe`. If
  the game crashes on launch or the controller stops working, run
  `uninstall.cmd` and the genuine system `XINPUT9_1_0.dll` takes over again
  immediately.
- **No tracking response.** Confirm OpenTrack is started and its **Output** is
  **UDP over network** to `127.0.0.1:4242`, not a mouse or joystick output.
  Check `HeadTracking.log` for the tracker connection line. If your game build
  is newer than any this mod knows about, the log says so and the mod stays
  dormant rather than hooking against stale offsets.
- **Nothing moves in third person (Steam).** On the Steam build the head
  rotation goes in where the first-person camera writes the frame, so it applies
  while you are looking through your own eyes and stands aside everywhere else -
  the third-person on-foot view, ships, exocraft and cutscenes run as the game
  intends. The Xbox Game Pass build is compiled differently and every camera writes
  through one shared routine, so there the view follows your head in third
  person too.
- **Jittery or unstable tracking.** Raise `[Smoothing] LocalSmoothing` if your
  tracker runs on this PC, or `RemoteSmoothing` if it is a phone or other device
  on the network. Both run `0.0` (none) to `1.0` (heavy). A phone sending raw
  pose direct is the usual cause; route it through OpenTrack so its filters can
  clean the feed up.
- **Wrong rotation axis.** If an axis moves the view the opposite way to your
  head, invert that axis in your tracker. The mod applies the pose as the
  tracker sends it and has no inversion or sensitivity settings of its own.
- **The weapon is off to one side when I aim down sights.** Your head is
  turned: the weapon stays on your aim and you are looking past it. Turn back to
  it, or move your aim to where you are looking.
- **Filing a bug report.** Set `[Debug] Diagnostics=true` in `HeadTracking.ini`,
  relaunch, reproduce, and attach `HeadTracking.log`.

## Updating

Download the new release and run `install.cmd` again. Your config is preserved.

## Uninstalling

Run `uninstall.cmd`. This removes the mod DLL and its log files, and leaves
`HeadTracking.ini` in `Binaries` so your settings are there if you install again.
The shim is the whole mod here, so there is no separate loader left behind, and
the DLL and its logs come off whether or not the install marker is present.

## Building from Source

Requires Visual Studio 2022 with the C++ build tools, CMake 3.20 or newer, and
[pixi](https://pixi.sh).

```powershell
git clone --recursive https://github.com/itsloopyo/no-mans-sky-headtracking
cd no-mans-sky-headtracking
pixi run build-release
pixi run package
```

## Community & Support

- [Discord](https://discord.com/invite/dxyZdyFNT9) - setup help, bug reports,
  and new-release announcements
- [Lopari](https://lopari.app) - free Windows launcher with one-click install
  and launch of head-tracking mods
- [Headcam](https://headcam.app) - free app that turns your phone into a head
  tracker

## License

MIT License - see [LICENSE](LICENSE) for details.

## Credits

- No Man's Sky developed and published by Hello Games.
- [OpenTrack](https://github.com/opentrack/opentrack) for the tracking protocol.
- [MinHook](https://github.com/TsudaKageyu/minhook) for function hooking.
- [cameraunlock-core](https://github.com/itsloopyo/cameraunlock-core), the
  shared tracking core.
- See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for full attributions.

## Disclaimer

This mod is not affiliated with, endorsed by, or supported by Hello Games. Use
at your own risk.
