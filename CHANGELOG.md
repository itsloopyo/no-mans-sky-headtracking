# Changelog

## [0.1.0] - 2026-09-22

### Added

- Xbox Game Pass support. The Game Pass copy ships a different binary from the
  Steam one - built with far less inlining, so the addresses the mod pins are in
  different places and the camera write the Steam build folds into each camera
  behaviour is a single shared routine here. It now has a build profile of its
  own, and the installer and the dev deploy find it on whichever drive the Xbox
  library lives on rather than assuming C:. One consequence worth knowing: on
  Game Pass the view follows your head in third person as well as first, where
  on Steam it applies in first person only.
- A profile for the 2026-09-10 Steam build. The game had patched underneath the
  mod, which left head tracking dormant on that copy; it works again.
- A profile for the 2026-09-17 Steam build, for the same reason: the game had
  patched again and head tracking had gone quiet on that copy.

### Fixed
- The crosshair marks the shot on Xbox Game Pass. It stayed at the middle of
  the screen there while the round went where your mouse was pointing, which
  made decoupled aim worse than useless on that build.
- Head tracking pauses for menus again on Xbox Game Pass. The menu check was
  reading an address that moved between Game Pass builds, so it either reported
  a menu open forever or not at all.
- Head yaw follows the walking horizon on foot and stays camera-local in a ship
  on Xbox Game Pass, as it already did on Steam.

- Head tracking works again on Xbox Game Pass. That copy had updated to package
  7.3.0.0 and the mod stood down rather than hook addresses it did not
  recognise; it has a profile for the new build now. Four things were wrong
  behind that, and all four are fixed: the camera write is a single shared
  routine on Game Pass rather than one built into each camera mode, so the mod
  was looking for it in the wrong shape; the check that decides when you are in
  the game ran once while the game was still starting, found nothing, and never
  looked again; the retry it now does would have found nothing either, because
  it was looking at a stale picture of memory; and the menu check was reading
  the wrong place and reported a menu open on every frame of play, which held
  tracking off even though everything else was working.
- Aim is decoupled on Xbox Game Pass. Your head moves the view and your mouse or
  controller keeps the aim, which that build has never done before. It works by
  writing the head-tracked camera where the game draws from and the clean camera
  where the game aims from, so nothing is substituted or copied.

  One thing to watch for: the renderer and the rest of the game are given
  different copies of the camera to make this work, so if scenery pops in or out
  at the edges of a hard head turn, that is why - please report it.

- Fixed an intermittent crash while head tracking was active.
- On the 2026-09-17 Steam build, walking direction follows mouse or controller
  aim when you turn your head.
- On the 2026-09-17 Steam build, ship sights keep their normal controller
  steering behavior while head tracking is enabled.
- On the 2026-09-17 Steam build, head yaw follows the walking horizon on foot
  and stays camera-local in a ship or during spacewalks.
- On the 2026-09-17 Steam build, reticle correction now follows the visible
  reticle through weapon switches and HUD reloads, including the station dot.
  Ship sights that already follow the aim are no longer shifted twice.
- On the 2026-09-17 Steam build, the mining beam now follows your mouse or
  controller aim when you turn your head. The reticle moves with that aim.
- Fixed the black world rendering on the 2026-09-17 Steam build while keeping
  the view and culling aligned with head tracking.
- The mod read the field of view from the wrong place on every build since the
  2026-09-10 update, reporting about one degree instead of the 75 the game was
  actually rendering. The camera class had been rearranged and the read was
  still using the old layout. Nothing on screen depended on it yet, so this is a
  correction to the diagnostics the log prints rather than to what you see.

- The Game Pass build rendered as flashing colour with almost nothing but the
  HUD legible. Where the mod substitutes its own copy of the camera for the
  callers that ask the engine for it, that copy was five rows long, which is all
  any Steam build reads. The Game Pass build inlines far less, so more of the
  engine reaches the camera that way, and something there reads past the rows
  and got whatever followed in memory - which smeared the whole frame into
  streaks at any head pose, centred included.

  Lengthening the copy fixed it on foot and it came back in a ship, so the copy
  is not made at all on that build now. It is a snapshot taken at the engine's
  camera commit and the engine reads it frames later, so any field that moves in
  between is stale in it however long it is. Game Pass went without decoupled aim
  for a while because of that; it has it again now by a different route, which is
  the entry above.

  Checked on the Game Pass copy on foot in first and third person, across a
  load, over a scan pulse, in the cockpit, and in flight.
- Head tracking stayed dormant on both the Steam and Game Pass copies of the
  current game version. No Man's Sky re-declared its camera class in the
  September update - the camera's transform moved to the start of the object,
  the object grew, and an accessor was inserted ahead of the one the mod hooks -
  so the mod refused to engage rather than hook the wrong function. The vtable
  slot it uses is now recorded per build instead of being a fixed number.

### Changed

- `pixi run install` and `pixi run deploy` now write to every copy of the game
  on the machine instead of the first one detection returns. Owning it on Steam
  and Game Pass at once is ordinary, and deploying to one while launching the
  other looks exactly like a broken build.

## [0.0.0] - 2026-09-05

### Fixed

- The crosshair moved twice as far as it should, so instead of sitting on the
  shot it slid past it in the direction opposite the head turn, and past about
  ten degrees it left the frame altogether. NMS asks for more than one reticle
  in a frame - in a ship it wants the multi-tool's and the ship gun's - and the
  correction was being applied to each of them, once per element. Only the first
  is corrected now; the rest are put back where their layout had them. Measured
  in the cockpit against the movement of the world itself: 221 px of crosshair
  against 222 px of view at 8 degrees of head yaw, 450 against 454 at 16, and
  700 against 695 at 24.
- The aspect ratio the crosshair is placed with is now read from the engine's
  own render size rather than measured off the game window. They are different
  rectangles - a window carries a caption and a border, and the engine renders
  at whatever size the upscaler asked for - and the ratio between them scales
  every horizontal correction.

### Added

- The crosshair now follows the shot. No Man's Sky draws its reticle in the
  middle of the frame and never gives it a screen position, so once head
  tracking turned the view off the gun it stopped marking where the rounds
  went. The mod projects the clean aim direction through the camera the frame
  was drawn from and writes the result into the reticle's own NGui layout
  position, so the game's own crosshair moves onto the shot. Measured in game:
  a forced quarter-frame step landed the reticle exactly where it was asked to,
  in all four directions. Turn it off with `[Reticle] FollowAim=false`.
  It corrects for rotation only - a lean also moves the eye off the gun, and
  correcting that needs the distance to whatever the round is about to hit,
  which nothing in the mod can ask the engine for.

- The frame-phase log line now reports the field of view the engine is
  rendering with, read live from two places because the game splits it: the
  live camera's own field (`+0xB0` of the camera object the manager hands out)
  never leaves 75, and the player's Options > Camera setting arrives as a
  global multiplier. No field-of-view key was added to `HeadTracking.ini` -
  the game already owns that control, and nothing in the mod's maths takes an
  FOV term.
- In windowed mode the game window is centred on the monitor it opened on,
  once, after its rect has stopped moving. A window that already fills the
  work area, or that the game centred itself, is left where it is.
- Head tracking now actually moves the view. The renderer does not read the
  camera transform copy the mod was hooking; it reads the live transform the
  camera manager hands out by pointer. The head rotation is written into that
  live transform immediately after the engine commits the frame's camera to it
  (RVA `0x00621E2B` on the 2026-06-18 Steam build), which is after the engine's
  own write and before anything draws. Every other consumer of the same camera,
  aim and raycasts included, is handed the untouched copy on its own call.
- 6DOF position: the tracked offset is applied to the camera position in the
  clean camera's own axes, so leaning follows the body rather than the
  head-rotated view.

### Changed

- The camera transform copy accessor is now hooked for diagnostics only. It
  used to receive the head rotation, which moved nothing on screen and did
  hand a tracked camera to the gameplay systems that read it.
- A build with no matching profile now leaves head tracking dormant and says
  so. The renderer's call site cannot be resolved from RTTI the way the vtable
  slot can, so an unknown build gets no tracking rather than a wrong camera.
- Smoothing is now two INI keys under `[Smoothing]`: `LocalSmoothing` (default
  `0.0`) for a tracker running on this machine, and `RemoteSmoothing` (default
  `0.15`) for a tracker on a remote network device. The value is chosen per
  connection from the packet source address and re-evaluated every frame, so
  swapping a local OpenTrack instance for a phone on WiFi needs no restart.
- The early-load diagnostic `XINPUT9_1_0_diag.txt` is truncated on the first
  write of each launch instead of appended to forever, so it holds only the
  session being reported.

### Fixed

- Every number in `HeadTracking.ini` is now read through the shared config
  guards instead of `IniReader`'s raw accessors. `UDPPort=abc` bound port 0 -
  an ephemeral port no tracker sends to - because `ReadInt` hands back 0 rather
  than its default on a value it cannot parse, and `UDPPort=70000` truncated to
  4464. A negative `LimitX/Y/Z` inverted the bounds of the position clamp, which
  then returned the same offset for every input and read as tracking having
  jammed. `nan` or `inf` in any sensitivity or limit skipped the range checks
  entirely - every comparison against NaN is false - and reached the camera
  basis. `LimitZ=0,40`, a European decimal comma, parsed as the prefix `0` and
  sat inside the valid range with nothing in the log. All of these now fall back
  to the shipped default and say so.
- A bool or an ADS mode with a trailing `; comment` no longer reverts to its
  default in silence. `GetPrivateProfileStringA` does not strip inline comments
  and the value was matched whole, so `AutoEnable=false ; off for now` left
  tracking enabled.
- A hotkey the OS can never report is refused instead of registered. `ReadHex`
  is a prefix parse, so `ToggleKey=End` read as `0x0E` and `ToggleKey=0x230`
  read as `0x230`; both bound a code `GetAsyncKeyState` never returns, and the
  key silently did nothing.
- The memory watch and the weapon decoupler no longer share a CPU debug
  register. Both are re-armed across every thread on a timer, so turning
  `[Debug] ReadWatch` on stopped the multitool being re-placed onto the clean
  camera and fed the decoupler's own breakpoint hits to the watch's report. The
  watch moved to the free slot 2.
- `[Debug] ReadWatch` and `WriteWatch` set together started two watch threads
  over one set of globals: the second flipped the first into write-only mode,
  overwrote the vectored-handler registration so one was leaked and the other
  removed out from under a live burst, and both fought for the same register.
  The second now stands down and says why.
- Uninstalling the commit hook or the weapon decoupler no longer removes its
  vectored exception handler while the hardware breakpoints are still armed.
  The arming thread only cleared them after a two-second sleep, so any thread
  reaching the trapped instruction in between raised an unhandled
  `EXCEPTION_SINGLE_STEP`. Both now wait for the disarm before removing the
  handler.
- `[Debug] Diagnostics` no longer runs the tracking pipeline from a second
  thread. The camera transform copy accessor asked `GetProcessedRotation` for a
  fresh pose every 600 calls, which ticks the frame clock and advances the
  session's interpolation and smoothing state - work documented as once per
  render frame, and the copy accessor is not the commit thread. It now reports
  the pose the commit hook last applied.
- The log file now opens before the config is read. `LoadFromIni` runs during
  bootstrap and its warning about a retired `[Smoothing] Factor` key was written
  to a log that was not open yet, so the user saw nothing.
- A retired smoothing key no longer silences the warning for the other one. A
  user upgrading can have both, and a single process-wide latch reported the
  first and dropped the second.
- `[Debug] Diagnostics` is in the shipped `HeadTracking.ini` and the README. It
  gates every discovery log line, and the shipped INI carried no `[Debug]`
  section, so nothing pointed at it.
- The discovery call-site table is guarded by a mutex. It exists to find out
  whether a second thread calls the camera transform getter, and two racing
  appends could both pass the bounds check and leave the report reading past the
  end of the array.
- The shipped INI described smoothing 1.0 as a "~5s settle". It is a 10 second
  time constant; five seconds only reaches about 39% of the way.

### Removed

- Removed `[Network] BindAddress`. It was read from the INI, written into the
  seeded file and documented, but never applied: `UdpReceiver::Start` takes a
  port and nothing else, and the receiver listens on every interface.
- Removed the `[Debug] SourceInjection`, `BasisScan` and `BasisInjectCycle`
  keys and the `Ctrl+Shift+U` call-site chord. Injecting into the live camera
  transform is what the mod does now rather than an experiment, and the
  memory scanner that looked for the render camera is superseded.
- Removed the yaw-mode toggle (`Page Down`, `Ctrl+Shift+H`, and the
  `[Hotkeys] ToggleYawModeKey` INI key). It flipped and logged a flag that
  nothing read, so it never changed how the camera was composed.
- Removed recentring from the mod. The tracker owns the centre, so the `Home`
  hotkey, the `Ctrl+Shift+T` chord and the `[Hotkeys] RecenterKey` INI key
  have been removed and the mod now applies the tracker pose as absolute.
  Centre your view in your tracker app instead.
- Removed `[Smoothing] Factor` and `[Position] Smoothing`; both new parameters
  cover rotation and position.
- Removed the hidden 0.15 baseline smoothing floor, so a tracker on this
  machine gets zero-latency tracking by default.
