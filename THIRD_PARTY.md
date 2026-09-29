# Third-party software

Adapter sources marked `SPDX-License-Identifier: Apache-2.0` retain that license;
the license text is in LICENSE. The nRF BAP sequencing follows Zephyr's
`bap_unicast_client` sample (Nordic Semiconductor, 2021–2024).

SDKs and codec libraries are downloaded separately, not redistributed in this
repository. Their original license terms apply independently:

- [ESP-IDF v6.1](https://github.com/espressif/esp-idf/tree/v6.1): Apache-2.0 and
  the individual third-party licenses in that SDK.
- [esp_audio_codec 2.6.2](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.6.2):
  Espressif's component license and accompanying third-party notices.
- [nRF Connect SDK 3.0.1](https://github.com/nrfconnect/sdk-nrf/tree/v3.0.1):
  licenses in sdk-nrf and its west manifest dependencies, including Zephyr,
  Nordic's controller and LC3 libraries. Nordic-only binary components remain
  subject to their vendor restrictions; this repository does not relicense them.
- Diagnostic Python packages and SEGGER J-Link software retain their licenses.

This is a standalone adapter application, not a distribution of the earables'
firmware. No earable firmware image or SDK binary is included.
