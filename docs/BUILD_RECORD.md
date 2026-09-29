# Fresh build record

Date: 2026-09-29. Host: macOS arm64. Both firmwares were rebuilt in fresh build
directories inside this public devkit repository, using the existing pinned SDK
installations. No target was flashed by these checks and no earable firmware was
changed. The firmware and common sources match the original devkit snapshot;
the README and serial monitor presentation were improved for publication.

- ESP-IDF 6.1 build: PASS. Application 937,856 bytes, within its 3 MiB partition.
- NCS 3.0.1 sysbuild: PASS for application and network core.
- nRF app: 403,224 bytes flash / 353,888 bytes RAM (77.14% of configured RAM).
- Compiler logs: no compiler warnings or errors. ESP-IDF emitted a shell
  autocompletion warning for macOS's Bash 3.2 and upstream Kconfig notes; these
  did not prevent a successful build.
- Host identity/SPI integrity test: PASS with address/undefined-behavior sanitizers.
- Host microphone processing: PASS at 8 and 16 kHz, measured nominal gains
  11.696 / 11.834 dB, limited peak 30,000, mute and silence checks passed.
- Python syntax checks: PASS for tools, bring-up verifier and ATT hook.

Build files are ignored rather than committed. SHA-256 for the checked outputs:

| Output | SHA-256 |
|---|---|
| `build/bridge-esp32-aac/openearable_adapter.bin` | `904d992677f9082937057f26b262c461ee42987218ce82759e1630e72a1bd06d` |
| `build/bridge-nrf/merged.hex` | `ca928ec3a84818b11de147ccde2928bc3d8ed4e73b02b561ed7cc297726289e1` |
| `build/bridge-nrf/merged_CPUNET.hex` | `d6bdcba1c38443655daf12d2ff7f836c6580bf2e3deeefc44b598a509f3fefb1` |

These are provenance hashes for this build, not a promise of byte-for-byte
reproducible binaries; paths and build metadata can change output hashes.
Both nRF images must be flashed using the top-level sysbuild directory. Its
generated flash order is `hci_ipc` followed by `nrf`.

These checks verify compilation and host-side processing only. See
VALIDATION.md for the separate physical-prototype measurements and limits.
