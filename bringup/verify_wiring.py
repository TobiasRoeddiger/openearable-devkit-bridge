#!/usr/bin/env python3
"""Exercise all nine bridge signals without enabling the Bluetooth radios.

Requires the matching WIRING_V1 firmware on both development boards. Outputs
return to inputs on exit. A passing result verifies static logic levels only;
it is not an I2S clock-integrity or SPI-throughput qualification.
"""
import argparse
import contextlib
import json
import re
import time
from pathlib import Path

import serial


class Board:
    """A specifically selected USB serial port running our diagnostic protocol."""

    def __init__(self, port, identity):
        self.identity = identity
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.15)
        self.serial.dtr = identity == "NRF"
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()

    def close(self):
        self.serial.close()

    def command(self, text, prefix):
        self.serial.reset_input_buffer()
        self.serial.write((text + "\n").encode("ascii"))
        deadline = time.monotonic() + 3
        lines = []
        while time.monotonic() < deadline:
            line = self.serial.readline().decode("ascii", errors="replace").strip()
            if line:
                lines.append(line)
            if line.startswith(prefix):
                return line
            if line.startswith("@ERROR"):
                raise RuntimeError(line)
        raise RuntimeError(f"{self.identity}: no {prefix!r} response: {lines}")

    def state(self):
        line = self.command("READ", f"@STATE {self.identity} ")
        values = dict(re.findall(r"([A-Z]+)=(-?[0-9a-f]+)", line))
        return {key: int(value, 10 if key in ("VDD", "DRIVE") else 16)
                for key, value in values.items()}

    def safe(self):
        if self.command("SAFE", "@SAFE ") != "@SAFE 0":
            raise RuntimeError(f"{self.identity}: cannot release outputs")

    def drive(self, value):
        reply = self.command(f"OUT {value:x}", "@OUT ")
        if reply != "@OUT 0":
            raise RuntimeError(f"{self.identity}: output refused: {reply}")


class RttBoard(Board):
    """The same line protocol through the DK's built-in SWD debugger."""

    def __init__(self, serial_number, address):
        import pylink
        self.identity = "NRF"
        self.probe = pylink.JLink()
        self.probe.open(serial_no=serial_number)
        self.target_mv = self.probe.hardware_status.VTarget
        if not 3100 <= self.target_mv <= 3450:
            self.probe.close()
            raise RuntimeError(f"Debugger reports target voltage {self.target_mv} mV")
        self.probe.set_tif(pylink.enums.JLinkInterfaces.SWD)
        self.probe.connect("NRF5340_XXAA_APP", speed=1000)
        if self.probe.halted():
            self.probe.close()
            raise RuntimeError("nRF is halted; run the diagnostic firmware first")
        self.probe.rtt_start(block_address=address)
        time.sleep(0.1)

    def command(self, text, prefix):
        self.probe.rtt_read(0, 4096)
        data = (text + "\n").encode("ascii")
        written = self.probe.rtt_write(0, data)
        if written != len(data):
            raise RuntimeError("RTT command was not accepted in full")
        deadline = time.monotonic() + 3
        received = bytearray()
        while time.monotonic() < deadline:
            received.extend(self.probe.rtt_read(0, 4096))
            for line in received.decode("ascii", errors="replace").splitlines():
                if line.startswith(prefix) and received.endswith(b"\n"):
                    return line
                if line.startswith("@ERROR"):
                    raise RuntimeError(line)
            time.sleep(0.005)
        raise RuntimeError(f"NRF: no {prefix!r} reply via RTT: {received!r}")

    def close(self):
        self.probe.rtt_stop()
        self.probe.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--esp", required=True)
    parser.add_argument("--nrf-probe", type=int, required=True)
    parser.add_argument("--rtt-address", type=lambda x: int(x, 0), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=5)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    result = {"started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
              "esp_port": args.esp, "nrf_probe": args.nrf_probe,
              "checks": [], "passed": False}
    boards = []
    try:
        esp = Board(args.esp, "ESP")
        boards.append(esp)
        nrf = RttBoard(args.nrf_probe, args.rtt_address)
        boards.append(nrf)
        for board in boards:
            board.state()  # Establish identity before issuing mutations.
            board.safe()
        result["initial_nrf"] = nrf.state()
        result["initial_esp"] = esp.state()
        vdd = result["initial_nrf"]["VDD"]
        if not 3100 <= vdd <= 3450:
            raise RuntimeError(f"nRF VDD is {vdd} mV; need 3100..3450 mV before driving signals")
        print(f"nRF supply: {vdd} mV", flush=True)
        for source, destination, count in [(esp, nrf, 4), (nrf, esp, 5)]:
            for board in boards:
                board.safe()
            mask = (1 << count) - 1
            patterns = [0, mask] + [1 << i for i in range(count)]
            patterns += [mask ^ (1 << i) for i in range(count)]
            for iteration in range(args.rounds):
                for pattern in patterns:
                    source.drive(pattern)
                    time.sleep(0.01)
                    observed = destination.state()["I"]
                    result["checks"].append({"source": source.identity,
                        "round": iteration, "sent": pattern, "received": observed})
                    if observed != pattern:
                        raise RuntimeError(f"{source.identity}->{destination.identity}: "
                            f"sent 0x{pattern:02x}, received 0x{observed:02x}")
            source.safe()
            print(f"PASS {source.identity}->{destination.identity}: {count} signals, "
                  f"{args.rounds * len(patterns)} patterns", flush=True)
        result["passed"] = True
        nrf.command("LEDS e", "@LEDS ")
        print("PASS: all nine signals; LEDs 2-4 on, LED 1 heartbeat", flush=True)
    except Exception as error:
        result["error"] = str(error)
        raise
    finally:
        for board in boards:
            with contextlib.suppress(Exception):
                board.safe()
            board.close()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
