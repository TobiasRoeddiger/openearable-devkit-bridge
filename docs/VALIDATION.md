# Validation and limitations

## Fresh flash and hardware retest — 2026-09-29

The public repository firmware was flashed onto the ESP32 and standard nRF5340
DK and exercised with both USB-powered earables. Pairing, two-ear LC3, SBC,
HFP and both BLE sensor proxies worked, but the strict audio-integrity test
failed. See [the measured retest](HARDWARE_RETEST.md) for the counters and two
adapter fixes made during verification. Earable firmware was unchanged.

## Physical prototype results before this repository snapshot

These observations were made on the original devkit assembly, not on a new
custom PCB. They are scoped observations, not a lossless or production claim.

- Static wiring: all 220 bidirectional patterns passed at approximately 3.3 V.
- macOS recognized the adapter as headphones with A2DP output and HFP input.
  GAP name and hardware revision came from the connected earable and survived
  adapter restart. Both playback channels were confirmed audible.
- AAC-first, forced SBC, HFP microphone input and return to AAC passed short
  source/ESP tests. Evaluated AAC and SBC windows were about 30 seconds each;
  HFP about 15 seconds. They showed no new RTP gaps, decode errors, PCM queue
  failures or I2S failures. The A2DP stimulus was digital silence, not a music
  stress test; its low compressed bitrate is not representative of music.
- BLE proxy: 12 services and all 20 readable characteristics passed. Both
  identities streamed IMU data, including repeated start/stop and reconnects.
  The unchanged macOS app displayed live sensor traces. This does not prove
  every app command or firmware-update operation is transparent.
- Earlier one-ear bidirectional LE Audio ran 120 seconds / 12,001 packets each
  way without new send, queue or concealment errors.
- A later 60-second concurrent two-ear audio/sensor test showed roughly
  0.48% / 0.69% invalid playback packets at the ears and 1.39% microphone
  concealment. SPI/I2S error counters did not explain these losses. Earable
  playback buffer overruns were also observed. Radio scheduling remains open.
- Actual microphone data reached the host, but acoustic speech quality,
  sensitivity and full-volume behavior remain unqualified. No calibrated
  Android loudness comparison was completed.

Current LC3 settings: playback 48 kHz / 120 bytes per 10 ms per ear; microphone
16 kHz / 40 bytes per 10 ms per ear. Both directions request five retransmissions
and 20 ms transport latency. The nRF application CPU runs at 128 MHz. The wire
uses 48 kHz I2S, 32-bit slots with signed 16-bit samples in the high half.

No earable firmware was modified. No universal Android/iOS compatibility,
zero-loss link, complete BLE transparency or secure update flow has been
established by these observations.

## Repository snapshot checks

See BUILD_RECORD.md for fresh build and host-test results from the independent
repository directory. Rebuilding does not revalidate the over-the-air path.
