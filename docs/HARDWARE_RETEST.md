# Hardware retest — 2026-09-29

**Result: functioning prototype, not a clean audio-integrity pass.** These are
measurements from the connected ELEGOO ESP32-WROOM-32 and standard PCA10095 DK,
using the firmware in this repository. The host was macOS; both HW 2.0.1
earables were USB-powered and observed through separate J-Links.

## Confirmed

| Check | Result |
|---|---|
| ESP flash | Bootloader, partition table and application written and verified |
| DK flash | Application and network core written and verified; target 3.3 V |
| Pair identity | Same CSIP set, ranks 1/2, left/right audio locations |
| Host identity | macOS headphones; name and HW revision read from earables |
| LE Audio | Both ears connected, bidirectional LC3 |
| SBC fallback | 30.04 s evaluated window, 48 kHz stereo, 327 kbit/s encoded payload; no new monitored host/ESP errors |
| HFP microphone | 15.03 s evaluated window, 16 kHz mono, nonzero microphone samples; no new monitored host/ESP errors |
| BLE proxy, each side | 12 services, 20 readable characteristics, no read errors |
| IMU stream | Left 373 / right 371 notifications in separate 15 s windows; original sensor settings restored |
| DK restart | Both ears reconnect; Mac bond retained without a new host-bond reset |

SBC used digital silence; HFP collected only aggregate level statistics, not
an audio recording. Neither test qualifies music quality, microphone sensitivity
or the radio path as lossless. Pair/reconnect/host-pair commands were exercised
through the adapter's diagnostic interface; physical five-second button timing
was checked in source, not mechanically actuated.

## AAC stress result

A quiet changing stereo multitone ran for 60 s. IMU streaming on the left proxy
started near the end. Startup and flashing counters were excluded by taking
deltas within the measurement window.

- ESP Bluetooth: AAC-LC, 48 kHz stereo, about 244 kbit/s payload. In the 55.15 s
  logged window: **zero RTP gaps/reorders, decode errors or encoded queue drops**.
- ESP playback: **27,648 decoded PCM frames dropped**, no PCM underflows;
  **2 return-wire padding errors**. Transport/decoder success therefore does
  not imply that every decoded sample reached the DK.
- DK: 12,002 LC3 packets transmitted and 12,002 received; no send errors,
  SPI errors or PCM restarts. However, **42 playback-wire padding errors**,
  **39 invalid microphone packets and 39 concealments**, plus 33 late/missing
  microphone playout events on the left, failed the strict check.
- Earable counters in an overlapping 60.82 s window: left 6,052 / right 6,084
  playback packets, **zero new invalid or size-mismatched packets**; left
  nevertheless reported **6 playback buffer overruns**, right zero.

Padding errors concern the unused lower 16 bits of the 32-bit I2S slots. They
are still integrity failures; their cause has not been isolated. These data
localize problems beyond the Classic Bluetooth decoder but do not establish
one common cause for the PCM, I2S and LE Audio failures.

A second 40 s multitone run after restarting both adapters had no new ESP PCM
drops, no radio concealments/invalid packets, and no playback buffer overruns
at either ear. It still failed integrity: **20 DK playback-wire padding errors
and 1 ESP return-wire padding error** (ESP window 35.09 s). Thus restarting
improved this run but did not produce an error-free result. No IMU stream was
requested during this repeat.

## Adapter fixes found during testing

- Increased the DK crypto heap from 512 to 2,048 bytes, matching Nordic's
  NCS 3.0.1 unicast-client reference. CSIP previously failed to import an AES key
  with `PSA_ERROR_INSUFFICIENT_MEMORY`; the full pair connected afterward.
- Load saved settings before starting SPI status transmission. Previously a
  temporary zero reset epoch at boot could erase the ESP's host bonds. A real
  DK restart after the fix preserved the Mac bond and restored both ears.

Both fixes are confined to the DK firmware. No earable was flashed, erased or
patched. Application-image comparisons before/after were identical for both
earables, including the same nine pre-existing ELF-versus-flash padding bytes.
The app was unchanged. AAC-first policy was restored after the SBC test.

Android/iOS, arbitrary replacement sets, all app functions and long-duration
audio remain unqualified. Raw bench logs stay local because they contain
device identifiers; the aggregate results above are the public record.
