# OpenEarable Devkit Bridge

> [!WARNING]
> **Experimental prototype / proof of concept:** a Bluetooth Classic audio to
> LE Audio adapter built from two development boards. This is not a finished
> product. Audio dropouts have been measured; microphone quality, complete BLE
> transparency and Android/iOS compatibility are not fully validated. Do not
> expect lossless or universally compatible operation. Check the wiring and
> power settings below before connecting the boards, and start at low volume.

Firmware for an **original ESP32-WROOM-32 development board** (ELEGOO WiFi + BT,
4 MB flash) and a **standard nRF5340 DK / PCA10095**. The ESP32 receives Classic
Bluetooth audio from a computer or phone; the nRF5340 sends LE Audio to an
existing left/right OpenEarable pair and proxies its BLE sensor services.
The earables and their app keep their existing firmware/software. Only the
ESP32 and the nRF5340 DK are flashed. The ESP32 advertises a headphone-class
Classic Bluetooth device; this is not a USB sound card.

**Status: working development prototype, not a qualified production release.**
macOS AAC, SBC, microphone transport and both earables have been exercised.
Concurrent two-ear audio/sensor operation still has measured radio losses.
Android/iOS, acoustic microphone quality, all app functions and firmware updates
through the adapter are not yet qualified. See [validation](docs/VALIDATION.md).

```text
Computer / phone  <-- A2DP AAC-LC or SBC / HFP -->  ESP32
                                                      |
                                            I2S audio + SPI control
                                                      |
                                                  nRF5340 DK
                                                   /       \
                                           LE Audio LC3 + BLE
                                                /             \
                                         Left earable      Right earable

OpenEarable app  <-- BLE per-ear proxies on nRF5340 -->  Original earables
```

AAC is advertised first; SBC remains available. The host chooses the codec.
AAC-LC supports 44.1/48 kHz, mono/stereo and VBR up to an advertised 320 kbit/s
limit. This is not the measured or fixed transmission bitrate. Music remains
stereo; HFP calls use mono in each direction and mix the two earable microphones.
The LE Audio leg uses LC3 independently of the host's A2DP codec.

## What you need

- An **original ESP32-WROOM-32 board with 4 MB flash and Bluetooth Classic**.
  The prototype used an ELEGOO ESP32 WiFi + BT development board. ESP32-S2,
  ESP32-S3 and ESP32-C3 are not substitutes for this firmware.
- A **standard Nordic nRF5340 DK (PCA10095)**, not the nRF5340 Audio DK.
- Short jumper wires for the 12 connections below, two USB data cables for
  programming, and a multimeter to verify the supply voltage.
- A working, charged left/right OpenEarable pair with its existing LE Audio
  firmware. The pair must already belong to the same coordinated set.
- A computer or phone with Classic Bluetooth A2DP/HFP. The physical prototype
  was tested with macOS; other hosts need their own validation. BLE support and
  the OpenEarable app are needed to use the sensor proxy.

Use this order: **wire and check power → flash both boards → pair the earables
with button 1 → pair the host with button 2 → select the headphone output**.

```sh
git clone https://github.com/TobiasRoeddiger/openearable-devkit-bridge.git
cd openearable-devkit-bridge
```

## Connect the devkits

Disconnect USB power before wiring. Use the **ESP32 GPIO labels**, not physical
header pin numbers. In the nRF column, `P1.04` means GPIO port 1, pin 4;
`P3.3` means physical header P3, pin 3. The table is for the tested PCA10095
header arrangement; check its printed signal labels if your revision differs.

| ESP32 pin | nRF5340 DK connection | Purpose / direction |
|---|---|---|
| VIN, carrying USB 5 V | P20.9, VIN 3–5V | Board supply |
| 3V3 | P21.1, External supply + | nRF target supply, 3.3 V |
| GND | P21.2, External supply − | Common ground |
| GPIO26 | P1.04, P3.3 / D2 | I2S bit clock, nRF → ESP |
| GPIO25 | P1.05, P3.4 / D3 | I2S word clock, nRF → ESP |
| GPIO22 | P1.06, P3.5 / D4 | Playback PCM, ESP → nRF |
| GPIO35 | P1.07, P3.6 / D5 | Microphone PCM, nRF → ESP |
| GPIO18 | P1.15, P4.6 / D13 | SPI clock, ESP → nRF |
| GPIO23 | P1.13, P4.4 / D11 | SPI MOSI, ESP → nRF |
| GPIO19 | P1.14, P4.5 / D12 | SPI MISO, nRF → ESP |
| GPIO27 | P1.12, P4.3 / D10 | SPI chip select, ESP → nRF |
| GPIO33 | P1.11, P4.2 / D9 | Ready, nRF → ESP |

Configure the switches with power disconnected:

| DK switch | Position |
|---|---|
| SW6 | DEFAULT |
| SW8 | ON |
| SW9, nRF power source | VDD |
| SW10, VEXT → nRF | ON |

The ESP32's 3V3 rail powers the nRF target directly so both GPIO sides operate at
3.3 V. Its VIN must actually expose the board's USB 5 V; do not assume an
unrelated ESP board has the same power circuit. Never connect 5 V to P21, VDD or
any GPIO. Keep signal wires short, with nearby common-ground connections.
Verify approximately 3.3 V between P21.1 and P21.2 before running the bridge;
do not use the DK's default 1.8 V target configuration with these 3.3 V signals.

Use the DK's **J2 USB connector, marked IF MCU**, for its built-in debugger.
The nRF USB connector is not the programming connection used here. The ESP32's
own USB connector powers the assembly and programs the ESP32 through its UART
bridge. Keep the ESP powered while flashing the DK, because it supplies the
target's 3.3 V even when the debugger's USB cable is connected. After flashing,
J2 is only needed for debugging; the ESP USB supply can power both boards via
the listed wires.

Nordic documents the [direct target supply and SW9/SW10 settings](https://docs.nordicsemi.com/r/bundle/ug_nrf5340_dk/page/ug/dk/direct_supply.html)
and [P20 pin 9 VIN connection](https://docs.nordicsemi.com/r/bundle/ug_nrf5340_dk/page/ug/dk/ext_programming_support_p20.html).
The optional [electrical wiring diagnostic](docs/WIRING_TEST.md) tests the
inter-board connections before running either radio.

## Pair and use

1. Put the existing left/right earable set in pairing mode.
2. Hold **DK button 1 for 5 seconds**. Candidates and their connected GAP names
   must start with the exact prefix `OpenEarable`. The second member must match
   the CSIP set and have the opposite channel assignment.
3. Hold **DK button 2 for 5 seconds**. The ESP32 becomes discoverable for 180
   seconds. Pair it in the computer/phone's Bluetooth settings and select it as
   the audio output, and as input when using its microphone.
4. The audio device name is read from the earables, preferring the left member.
   Its hardware revision is read from the original BLE hardware characteristic;
   neither value is hardcoded. Hosts may cache an old name until re-paired.
5. For sensor data, grant the OpenEarable app Bluetooth permission and connect
   to the proxied left/right devices. The nRF advertises two BLE identities;
   sensor traffic does not travel through the host's Classic audio connection.

| Control | Action |
|---|---|
| Button 1, short press (50 ms–1 s) | Reconnect the saved earable set |
| Button 1, hold 5 s | Search for a replacement set |
| Button 2, hold 5 s | Open host pairing for 180 s |
| Both buttons, hold 10 s | Clear the adapter's host and earable bonds |

These are the DK's physical **BUTTON1 and BUTTON2**, not the ESP's BOOT/EN
buttons. Buttons 3 and 4 have no bridge action. Release the buttons after the
hold action; short button-2 presses have no assigned function. After clearing
bonds, forget the old entry on the host and repeat both pairing steps.

Replacement first looks outside the saved set; after 40 seconds without a new
candidate it falls back to the saved pair. Reset clears adapter storage only,
not the earables' own left/right set. Startup reconnects saved devices. If only
one earable was available initially, use a short button-1 press after the second
returns; adding it to a running single-ear stream is not yet automatic.

| DK LED | Meaning |
|---|---|
| 1 | Running heartbeat |
| 2 | Blinking: earable search; steady: an earable is connected |
| 3 | Blinking: host pairing; steady: host connected |
| 4 | LE Audio streams active; these may currently carry silence |

The app sees separate BLE identities for the two proxied earables. The Classic
headphone entry and BLE identities are not guaranteed to collapse into one
system device on every platform. Both boards must use the same SPI protocol
version; this snapshot uses version 2.

## Build and flash

The instructions and helper scripts are tested on **macOS with a POSIX shell**.
Linux should use equivalent SDK installations; native Windows instructions and
tool behavior have not been validated. Commands below run from the repository
root unless stated otherwise. No prebuilt firmware binaries are distributed.

### 1. ESP32: USB/UART programming

Use the original dual-mode ESP32, not ESP32-S2/S3/C3. Install Python 3.10+,
CMake 3.22+, Ninja and Git for the ESP build. The ESP project pins ESP-IDF **6.1**
and `espressif/esp_audio_codec` **2.6.2**. From this repository:

```sh
mkdir -p build
git clone --branch v6.1 --depth 1 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git build/esp-idf-source
IDF_TOOLS_PATH="$PWD/build/idf-tools" ./build/esp-idf-source/install.sh esp32
python3 tools/build_esp.py
python3 tools/build_esp.py --flash --port ESP_SERIAL_PORT
```

Replace `ESP_SERIAL_PORT` with the actual ESP32 port. The builder checks SDK
commit `fff9895c82d744c7237be8847347bdd1b07c6643` and refuses implicit flash targets.
For an existing installation, pass `--sdk PATH --tools PATH`. USB flashing writes
the bootloader, partition table and application; a blank ESP does not require a
previously installed software bootloader. The board's ROM downloader provides
initial programming. If automatic reset fails, hold BOOT, tap EN, then release
BOOT when connection starts. Close serial monitors before flashing.

On macOS, list candidate serial ports with `ls /dev/cu.*` and identify the one
that appears when you connect the ESP. A typical port is
`/dev/cu.usbserial-XXXX`; replace the placeholder, do not copy it literally.
Depending on the board's UART chip, its manufacturer's CP210x/CH34x USB driver
may be needed. The DK debugger serial number is a different identifier.

### 2. nRF5340 DK: built-in debugger, both cores

Install **nRF Connect SDK 3.0.1** and its matching toolchain. Open that SDK's
terminal environment so `west`, CMake, Ninja and the Arm toolchain are available.
Also install [Nordic nRF Util](https://www.nordicsemi.com/Products/Development-tools/nRF-Util/Download)
with its `device` command (`nrfutil install device`) and
[SEGGER J-Link software](https://www.segger.com/downloads/jlink/).
Use [Nordic's SDK installation guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html)
and select **3.0.1**, rather than a newer SDK.

Replace `/path/to/ncs/v3.0.1` below with the directory containing `nrf`,
`zephyr`, and `.west`. Find the DK's built-in debugger serial using
`nrfutil device list` or the number printed on the DK. Connect USB to **J2 / IF
MCU** and keep the ESP powered.

```sh
export NCS_WORKSPACE="/path/to/ncs/v3.0.1"
export ZEPHYR_BASE="$NCS_WORKSPACE/zephyr"
python3 tools/build_nrf.py --ncs "$NCS_WORKSPACE"
nrfutil device list
west flash -d build/bridge-nrf -r nrfutil --dev-id DK_PROGRAMMER_SERIAL
```

The nRF builder verifies the nrf and Zephyr revisions and builds **both
application and network cores** using sysbuild. Select the **DK's built-in
programmer** explicitly; never substitute an earable's external J-Link.
If a previously protected DK requires recovery, that erases the DK's old
firmware and settings. No command in these instructions targets the earables.
Flash the **sysbuild directory** shown above, not just its `nrf` application
subdirectory: the network-core image is required for LE Audio. There is no need
to connect a J-Link to either earable.

The nRF application uses a build-local Zephyr ATT hook for asynchronous proxy
responses; it does not patch the installed SDK. The current devkit build has no
implemented nRF update-via-ESP or complete FOTA flow.

### 3. First start

After flashing both boards, power-cycle the adapter. DK LED 1 should heartbeat.
Perform the two pairing steps above, then select the dynamically named
OpenEarable headphone entry in the host's sound settings. Opening its HFP
microphone switches the host into call mode; stereo A2DP music and HFP call
audio are different modes. Close applications using the microphone to return
to music mode. Begin playback at low volume.

## Diagnostics and source layout

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
| ESP flash does not connect | Correct USB data cable and serial port; close the monitor; use BOOT/EN as described above. |
| DK cannot be programmed | Use J2 / IF MCU, select the DK debugger serial, and check that ESP 3V3 still supplies P21. |
| Host does not see headphones | Pair the earables first, then hold button 2 for 5 s; discovery lasts 180 s. Forget stale host entries if the name changed. |
| No earables found | Charge both, disconnect competing hosts, enable their pairing mode, then hold button 1 for 5 s. Only the exact `OpenEarable` prefix is accepted. |
| Only one earable plays | Ensure the other is powered and belongs to the same pair, then briefly press button 1 to reconnect the saved set. |
| LEDs indicate a stream but there is no sound | Select the adapter as the host output and start playback; LED 4 can also mean a stream carrying silence. Check I2S wiring. |
| Stereo music changes quality when the microphone opens | The host has switched from A2DP to mono HFP call audio. Close microphone users to return to A2DP. |
| Audio drops out or microphone level is poor | Check supplies and short signal wires, capture ESP and nRF diagnostics, and consult the measured limitations below. Zero-loss or calibrated microphone performance is not established. |

For a reproducible issue report, include board models, host OS, SDK versions,
negotiated codec, button sequence and relevant counter changes. Remove personal
device names and Bluetooth addresses from logs before posting them publicly.

## Source layout and validation

- `firmware/esp32-idf`: AAC/SBC/HFP, I2S and SPI firmware.
- `firmware/nrf`: dual-core LE Audio and BLE proxy application.
- `common`: shared SPI frame, identity conversion and microphone processing.
- `bringup`: optional radio-free electrical diagnostic; see
  [its instructions](docs/WIRING_TEST.md).
- `docs/VALIDATION.md`: measured results, build record and remaining limits.

The adapter source is licensed under [Apache-2.0](LICENSE). SDKs, builds,
recordings, device bonds and programmer identifiers are not shipped in the
repository. See [third-party notices](THIRD_PARTY.md).
