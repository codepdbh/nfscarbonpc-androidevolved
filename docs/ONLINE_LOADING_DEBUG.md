# Online loading investigation, 2026-10-22

Status: reproduced, not fixed. Offline career reached driving after the separate `_CIpow` override. Joining a public NFSCO race repeatedly leaves the Android client on the animated loading screen.

## Evidence

- `work/device/online_live.log`: continuous ADB capture across diagnostic builds. UDP traffic continues on the NFSCO connection while the loading screen presents approximately 120 frames/s. Thus the no-frame stall detector does not fire.
- `work/device/game_globals.bin`: guest range `0x009c0000..0x00c2c000` captured from the second reproduced hang. `0x00a99bbc=6`; `0x00a99bb4` points to `TrackLoader::LoadHandler`; `0x00a99bb8=3`; `0x00a99b9c=0`. The value 4 at `0x00a99b94` is the size of a tree of VLT objects, **not a count of queued loader tasks**.
- Native frame samples resolve through `sub_007311B0`, `sub_00699230`, `sub_006B7930`, the palmont main-loop hook, and the boot worker. Worker traces show network sleeps, normal event waits, and a `sub_008D5CD0` SleepEx loop; no critical-section deadlock was established.
- The first LLDB attachment left the app paused while the client attempted to connect; Android subsequently recorded an input-timeout ANR. This ANR was a diagnostic side effect, not evidence for the original loading stall. Avoid a long debugger attachment. A short RSP snapshot succeeded, but memory transfers also pause the game and should not be used for continued capture.
- The latest build logs full failed file paths. No missing race asset was established before USB disconnected. Missing Discord IPC pipes are expected on Android.

## Installed diagnostic

`sampleOnlineLoading()` is opt-in via `/sdcard/nfsc/nfsc_online_trace`. It samples native frames and worker waits every ten seconds without stopping the process. When game state is 6, it also writes `/sdcard/nfsc/nfsc_online_state.bin` with the loader globals, palmont data, and up to 128 reachable heap blocks. Format: repeated little-endian `{uint32 address, uint32 byte_count, byte data[byte_count]}` records.

The automatic state snapshot was recovered from process 12566 after another reproduced load. The NFSCO main-thread work vector at `0x011ca0a0..a8` was empty. The loading flag `0x011cbf70` remained 1. No loader transition callback was registered at `0x00a99b9c` or `0x00a99ba8`. The user subsequently closed the game manually.

The next diagnostic adds full game globals, selected mod/streaming heap roots, bounded logging of indirect loader dispatches, and `nfsc_online_code.bin` (same record format) containing readable game executable pages. Compare the latter with `work/nfsco.analysis.exe` to identify code patches applied after startup that a static lift might miss. Also compare creating a test race with joining another host's race. Do not force the game-state variable or report the issue fixed without reaching the online track.

The chat duration change was separately verified in the phone's `NFSCO/messenger.json`: `display_time=20.0`, `fade_speed≈0.8`. The startup adjustment preserves longer durations and slower fades.
