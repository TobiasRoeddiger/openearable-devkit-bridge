# Diagnostics and troubleshooting

Run these commands from the repository root. Use the same SDK environment as for building.

```sh
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r requirements.txt
mkdir -p results
python3 tools/monitor_esp.py --port ESP_SERIAL_PORT --output results/esp.jsonl
```

Enter ESP commands on the monitor's standard input. `STATS` requests PCM and
I2S counters; periodic `BT_STATS` log lines report codec and transport counters.
`BT_STATS` is a log label, not a command. `CODEC SBC` restarts in SBC-only mode; `CODEC AUTO` restores
the default AAC-first/SBC fallback policy and restarts. Neither erases bonds.
`QUIT` closes the monitor.

For nRF RTT, install SEGGER's J-Link software and run from the NCS environment:

```sh
python3 tools/rtt.py --probe DK_PROGRAMMER_SERIAL \
  --elf build/bridge-nrf/nrf/zephyr/zephyr.elf --command RADIO --seconds 10
python3 tools/test_host.py
```

`--elf` must match the installed DK image. Use `--nm PATH` if the toolchain's
`arm-zephyr-eabi-nm` is not on PATH. Useful RTT commands: `PAIR`, `HOSTPAIR`,
`RECONNECT`, `SUSPEND`, `RADIO`, `CLOCK`. `RESET` clears adapter bonds. `TEST ON`
creates a diagnostic signal on the microphone-return wire; `TEST OFF` stops it.
Test mode is off at boot. Host tests check identity/frame integrity and the
microphone gain/limiter; they do not establish radio or acoustic performance.

## Troubleshooting

| Symptom | Check |
|---|---|
| ESP flash does not connect | Correct USB data cable and serial port; close the monitor; use BOOT/EN as described in the [README](../README.md). |
| DK cannot be programmed | Use J2 / IF MCU, select the DK debugger serial, and check that ESP 3V3 still supplies P21. |
| Host does not see headphones | Pair the earables first, then hold button 2 for 5 s; discovery lasts 180 s. Forget stale host entries if the name changed. |
| No earables found | Charge both, disconnect competing hosts, enable their pairing mode, then hold button 1 for 5 s. Only the exact `OpenEarable` prefix is accepted. |
| Only one earable plays | Ensure the other is powered and belongs to the same pair, then briefly press button 1 to reconnect the saved set. |
| LEDs indicate a stream but there is no sound | Select the adapter as the host output and start playback; LED 4 can also mean a stream carrying silence. Check I2S wiring. |
| Stereo music changes quality when the microphone opens | The host has switched from A2DP to mono HFP call audio. Close microphone users to return to A2DP. |
| Audio drops out or microphone level is poor | Check supplies and short signal wires, capture ESP and nRF diagnostics, and consult [measured limitations](VALIDATION.md). Zero-loss or calibrated microphone performance is not established. |

For a reproducible issue report, include board models, host OS, SDK versions,
negotiated codec, button sequence and relevant counter changes. Remove personal
device names and Bluetooth addresses from logs before posting them publicly.
