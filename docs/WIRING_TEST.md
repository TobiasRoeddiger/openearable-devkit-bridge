# Optional electrical diagnostic

This replaces the bridge temporarily on the **two devkits only**. It does not
use either radio. First wire and configure power as shown in the main README.

Build `bringup/nrf` with the NCS 3.0.1 environment:

```sh
west build -b nrf5340dk/nrf5340/cpuapp -d build/wiring-nrf bringup/nrf
west flash -d build/wiring-nrf -r nrfutil --dev-id DK_PROGRAMMER_SERIAL
```

Build `bringup/esp32` with Arduino CLI and Espressif Arduino core **3.3.11**:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build/wiring-esp bringup/esp32
arduino-cli upload --fqbn esp32:esp32:esp32 --input-dir build/wiring-esp \
  --port ESP_SERIAL_PORT bringup/esp32
```

Both images start with bridge signals as inputs. The nRF refuses to drive them
unless its measured supply is 3.10–3.45 V. Install Python diagnostic requirements
and SEGGER J-Link, then locate the RTT block in the exact diagnostic ELF:

```sh
arm-zephyr-eabi-nm build/wiring-nrf/zephyr/zephyr.elf | rg ' B _SEGGER_RTT$'
mkdir -p results
python3 bringup/verify_wiring.py --esp ESP_SERIAL_PORT \
  --nrf-probe DK_PROGRAMMER_SERIAL --rtt-address RTT_HEX_ADDRESS \
  --rounds 10 --output results/wiring.json
```

Use the actual DK programmer and the hexadecimal address from `nm`. No earable
J-Link is involved. The verifier exercises zero/one and walking-bit patterns,
releases the opposing outputs for each direction and tries to leave all signals
as inputs on exit. A passing result checks static levels, not I2S timing or RF.
Restore both bridge images using the main README after the test.
