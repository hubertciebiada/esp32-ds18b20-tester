# esp32-ds18b20-tester

[![build](https://github.com/hubertciebiada/esp32-ds18b20-tester/actions/workflows/build.yml/badge.svg)](https://github.com/hubertciebiada/esp32-ds18b20-tester/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: ESP32](https://img.shields.io/badge/platform-ESP32-blue.svg)](https://platformio.org/)
[![Framework: Arduino](https://img.shields.io/badge/framework-Arduino-00979D.svg)](https://platformio.org/frameworks/arduino)

A standalone workbench tester for DS18B20 sensors on the ESP32 — scans the
1-Wire bus, shows full ROM addresses and live temperatures on a local web page,
and identifies individual sensors with the warm-glass method (the hottest sensor
is highlighted).

Fully standalone — **no** Home Assistant / MQTT / ESPHome / cloud integration.
Full specification in [`SPEC.md`](SPEC.md).

## Preview

The web page auto-refreshes every second. The row with the highest temperature
is clearly highlighted — dip the sensors one by one into warm water and you
immediately see which ROM "jumped".

| Desktop | Mobile |
|---------|--------|
| [![Web UI – desktop](docs/img/web-desktop.png)](docs/img/web-desktop.png) | [![Web UI – mobile](docs/img/web-mobile.png)](docs/img/web-mobile.png) |

> Screenshots generated with mock data (script [`tools/screenshot.py`](tools/screenshot.py)) —
> it renders the real firmware front-end without a connected ESP32.

## Features

- Bus scan at startup and on demand (the **Rescan** button).
- Full 8-byte ROM address of every sensor in two formats:
  - readable (16 hex chars, uppercase, e.g. `28FF641E0016035C`),
  - ready to paste into ESPHome (`address`, e.g. `0x5c0316001e64ff28`).
- Non-blocking temperature reads (`millis()`, one shared Skip ROM conversion) —
  the web server stays responsive and there is no `delay()` in the main loop.
- Web page with 1 s auto-refresh: ROM / temperature / status table,
  **clear highlight of the hottest sensor** plus its ROM at the top.
- CSV export, copy the ROM list to the clipboard.
- Per-sensor diagnostics: `reset85` (85.00 °C), `disconnected` (-127 °C),
  `noread` (none/NaN). Faulty sensors are excluded from the maximum calculation.
- Likely-clone flag (ROM that does not match the `28-xx-xx-xx-xx-00-00-xx` pattern).

## Hardware

- ESP32 DevKit (`env:esp32dev`).
- 1-Wire bus on **GPIO4**, a single shared line.
- **2.2 kΩ** pull-up resistor between DQ and 3.3 V.
- 3-wire power (VDD / GND / DQ) at 3.3 V — **no** parasite power.

## Configuration

All settings live in the `#define` constants at the top of
[`src/main.cpp`](src/main.cpp): 1-Wire pin, resolution (12-bit by default),
maximum number of sensors, WiFi credentials.

By default the device starts as an **Access Point**:

| Parameter | Value |
|-----------|-------|
| SSID      | `DS18B20-Tester` |
| Password  | `tester1234` (change before use!) |
| Address   | `http://192.168.4.1` |

To use **STA** mode (join an existing network): set `USE_WIFI_STA` to `1` and
uncomment/fill in `WIFI_STA_SSID` / `WIFI_STA_PASS`. Read the IP address from
the serial monitor (115200 baud).

## Build and flash (PlatformIO)

```bash
pio run                 # compile
pio run -t upload       # flash the ESP32
pio device monitor      # view logs (115200 baud)
```

After flashing, connect to the `DS18B20-Tester` network and open `http://192.168.4.1`.

## HTTP endpoints

| Path          | Method | Description |
|---------------|--------|-------------|
| `/`           | GET    | Web page (HTML in PROGMEM) |
| `/data`       | GET    | JSON with the state of all sensors |
| `/export.csv` | GET    | Download the sensor list as CSV |
| `/rescan`     | POST   | Schedule a bus rescan |

## Pull-up selection

If sensors go missing during the scan, drop from 2.2 kΩ to **1.5 kΩ** — with a
long bus and many sensors a stronger pull-up pulls the line up faster. If that
does not help, split the sensors across **two separate buses** on different GPIOs.
