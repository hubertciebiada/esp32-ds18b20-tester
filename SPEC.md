# Project: DS18B20 sensor tester on ESP32 (standalone, with web UI)

## Goal
ESP32 firmware that acts as a workbench tester for dozens of DS18B20 sensors on
a single 1-Wire bus. It must detect every sensor, show their full ROM addresses
and live temperatures on a local web page, and support identifying "which ROM is
which physical sensor" with the warm-glass method. Do NOT integrate with Home
Assistant / MQTT / ESPHome — it must be fully standalone.

## Hardware context (already fixed, do not change)
- Board: generic ESP32 DevKit (env esp32dev).
- 1-Wire bus on GPIO4, a single shared line.
- 2.2 kΩ pull-up between DQ and 3.3 V.
- 3-wire power (VDD/GND/DQ), sensors at 3.3 V — NOT parasite power.
- Dozens of sensors (assume up to ~50) on the bus at once.

## Stack
- PlatformIO, arduino framework.
- Libraries: paulstoffregen/OneWire, milesburton/DallasTemperature,
  bblanchon/ArduinoJson @ ^7 (use v7 syntax: JsonDocument, to<JsonArray>(),
  add<JsonObject>()), ESP32Async/AsyncTCP, ESP32Async/ESPAsyncWebServer @ ^3.
- Deliver platformio.ini + src/main.cpp + a short README (build + flash).

## Functional requirements
1. Bus scan at startup: detect all sensors, count them.
2. For each sensor show the FULL 8-byte ROM address:
   - in readable format (16 hex chars, uppercase, e.g. 28FF641E0016035C),
   - and in a variant ready to paste into ESPHome/HA (with a 0x prefix).
   Verify the byte order matches how ESPHome writes the address field.
3. NON-BLOCKING temperature reads:
   - setWaitForConversion(false),
   - a single requestTemperatures() (Skip ROM + Convert T for all at once),
   - read the scratchpads only after the conversion time (750 ms for 12-bit),
   - the whole cycle on millis(), no delay() in loop — the web server must stay smooth.
4. WiFi in Access Point mode (SSID/password in constants at the top of the file),
   with an option to switch to STA mode (commented-out constants).
5. Asynchronous web server (ESPAsyncWebServer) serving:
   - the HTML page (in PROGMEM) with auto-refresh (fetch JSON every 1 s),
   - a /data endpoint returning JSON with the sensor list,
   - a /rescan endpoint to rescan the bus.
6. Web page:
   - table: ROM | temperature | status,
   - CLEAR highlight of the row of the sensor with the currently HIGHEST
     temperature, plus its ROM shown at the top (key for the warm-glass method —
     I dip sensors one by one into warm water and watch which one jumped),
   - an "Export CSV" button (file download),
   - a "Copy ROM list" button (to the clipboard),
   - a "Rescan" button (calls /rescan).
7. Per-sensor error detection and status flag:
   - 85.00 °C = no successful conversion / weak power (status reset85),
   - -127 °C = disconnected / no communication (status disconnected),
   - NaN / no reading (status noread),
   - EXCLUDE faulty sensors from the maximum calculation.
8. Likely-clone flag: a ROM that does not match the 28-xx-xx-xx-xx-00-00-xx
   pattern is flagged visually (do not reject it, only flag it).

## Non-functional requirements
- The code must compile under PlatformIO with no manual fixes.
- Configuration (1-Wire pin, SSID/password, resolution, max sensor count)
  in #define constants at the top of the file.
- Code comments in English.
- Sensor resolution configurable (12-bit by default).

## What NOT to do
- No HA / MQTT / ESPHome / any cloud integration.
- No localStorage/sessionStorage on the page (keep state in JS memory).
- No blocking delay() in the main loop.

## Additionally
Add a "pull-up selection" section to the README: if sensors go missing during
the scan, drop from 2.2 kΩ to 1.5 kΩ, or split into two buses on separate GPIOs.
Keep it short, one or two sentences.
