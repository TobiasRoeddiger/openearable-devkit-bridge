# OpenEarable Devkit Bridge

> [!WARNING]
> **Experimental prototype:** Bluetooth Classic audio → LE Audio using two
> devkits. Dropouts are still possible; microphone quality, complete BLE
> transparency and Android/iOS compatibility are not fully validated.
> Check the power wiring and start at low volume.

The ESP32 receives **AAC/SBC music or HFP calls**; the nRF5340 DK forwards
**LC3 audio** to an OpenEarable pair and exposes its BLE sensor services.
AAC is offered first, with SBC as fallback; the host selects the codec.
Music is stereo, calls are mono. The earables and app stay unchanged.

```text
Phone / computer ── Classic Bluetooth ── ESP32
                                          │ I2S + SPI
                                     nRF5340 DK ── LE Audio ── L / R earables
OpenEarable app ───────────── BLE ──────────┘
```

## Hardware and wiring

- Original **ESP32-WROOM-32**, 4 MB flash (tested: ELEGOO WiFi + BT).
  ESP32-S2/S3/C3 do not support this firmware's Classic Bluetooth path.
- Standard **nRF5340 DK / PCA10095**, not the Audio DK.
- An existing paired left/right OpenEarable set, short jumper wires and USB data cables.

**Disconnect power before wiring.** ESP numbers below are GPIO labels.
For the DK, `P1.04` is a GPIO; `P3.3` is header P3, pin 3.

| ESP32 | nRF5340 DK | Signal |
|---|---|---|
| VIN / USB 5 V | P20.9, VIN 3–5V | Board supply |
| 3V3 | P21.1, external supply + | Target supply |
| GND | P21.2, external supply − | Common ground |
| GPIO26 | P1.04 · P3.3 / D2 | I2S bit clock, nRF → ESP |
| GPIO25 | P1.05 · P3.4 / D3 | I2S word clock, nRF → ESP |
| GPIO22 | P1.06 · P3.5 / D4 | Playback, ESP → nRF |
| GPIO35 | P1.07 · P3.6 / D5 | Microphone, nRF → ESP |
| GPIO18 | P1.15 · P4.6 / D13 | SPI clock |
| GPIO23 | P1.13 · P4.4 / D11 | SPI MOSI |
| GPIO19 | P1.14 · P4.5 / D12 | SPI MISO |
| GPIO27 | P1.12 · P4.3 / D10 | SPI chip select |
| GPIO33 | P1.11 · P4.2 / D9 | Ready, nRF → ESP |

Set **SW6 = DEFAULT, SW8 = ON, SW9 = VDD, SW10 = ON**.
Measure approximately **3.3 V across P21**; do not use the default 1.8 V target
supply with these signals. Never connect 5 V to P21 or a GPIO. Confirm that
your ESP board's VIN actually exposes USB 5 V.

Program the DK through **J2 / IF MCU**. Keep the ESP USB connected: it supplies
the target's 3.3 V. After flashing, the ESP USB can power both boards.
[Nordic power guide](https://docs.nordicsemi.com/r/bundle/ug_nrf5340_dk/page/ug/dk/direct_supply.html)
· [P20 pinout](https://docs.nordicsemi.com/r/bundle/ug_nrf5340_dk/page/ug/dk/ext_programming_support_p20.html)
· [Optional wiring test](docs/WIRING_TEST.md)

## Build and flash

These commands are tested on macOS with a POSIX shell. Install Python 3.10+,
Git, CMake and Ninja. Only the **two devkits** are flashed.

```sh
git clone https://github.com/TobiasRoeddiger/openearable-devkit-bridge.git
cd openearable-devkit-bridge
```

### ESP32

Use **ESP-IDF 6.1**; the build pins the SDK and downloads codec component 2.6.2.

```sh
mkdir -p build
git clone --branch v6.1 --depth 1 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git build/esp-idf-source
IDF_TOOLS_PATH="$PWD/build/idf-tools" ./build/esp-idf-source/install.sh esp32
python3 tools/build_esp.py --flash --port ESP_SERIAL_PORT
```

Replace `ESP_SERIAL_PORT` with the ESP port (`ls /dev/cu.*` on macOS).
Close serial monitors first. If connection fails, hold **BOOT**, tap **EN**,
then release BOOT when flashing starts. The command installs the bootloader,
partition table and application; it also works on a blank ESP.

### nRF5340 DK

Install **nRF Connect SDK 3.0.1** and its matching toolchain using
[Nordic's guide](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/installation.html),
plus [nRF Util](https://www.nordicsemi.com/Products/Development-tools/nRF-Util/Download)
and [J-Link](https://www.segger.com/downloads/jlink/).
Run from the SDK's terminal environment, in this repository:

```sh
nrfutil install device
export NCS_WORKSPACE="/path/to/ncs/v3.0.1"
export ZEPHYR_BASE="$NCS_WORKSPACE/zephyr"
python3 tools/build_nrf.py --ncs "$NCS_WORKSPACE"
nrfutil device list
west flash -d build/bridge-nrf -r nrfutil --dev-id DK_PROGRAMMER_SERIAL
```

Replace the SDK path and select the **DK's built-in debugger serial**, never
an earable's J-Link. The top-level sysbuild directory flashes **both cores**.
Update-via-ESP and FOTA are not implemented; use USB/J-Link for updates.

## Pair and use

1. Power-cycle the adapter and put both earables in pairing mode.
2. Hold **DK button 1 for 5 s** to connect the earable set.
3. Hold **DK button 2 for 5 s**, then pair the adapter in the host's Bluetooth
   settings and select it as the headphone output.
4. Connect the two BLE proxies in the OpenEarable app for sensor data.

| DK control | Action |
|---|---|
| Button 1, short press | Reconnect the saved set |
| Button 1, hold 5 s | Search for a replacement earable set |
| Button 2, hold 5 s | Host pairing for 180 s |
| Both, hold 10 s | Clear adapter bonds; then forget its old host entry and re-pair |

These are **BUTTON1/BUTTON2 on the DK**, not ESP BOOT/EN. Buttons 3/4 are unused.
Only names beginning with `OpenEarable` are accepted; both ears must belong to
the same set. The host name and hardware revision come from the earables.
A replacement search falls back to the saved set after 40 s. If the second ear
joins late, briefly press button 1. Opening the microphone switches to mono HFP.

**LEDs:** 1 = heartbeat; 2 = earable search (blink) / connected (steady);
3 = host pairing (blink) / connected (steady); 4 = LE Audio streaming, including
silence. The app exposes left/right separately; audio and BLE may appear as
separate system devices.

## More

[Diagnostics and troubleshooting](docs/DIAGNOSTICS.md) ·
[Measured results and limitations](docs/VALIDATION.md) ·
[Build record](docs/BUILD_RECORD.md)

Firmware: [`firmware/esp32-idf`](firmware/esp32-idf) and
[`firmware/nrf`](firmware/nrf). Shared protocol: [`common`](common).
[Apache-2.0](LICENSE); SDKs and codecs retain their [own licenses](THIRD_PARTY.md).
