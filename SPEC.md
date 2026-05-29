# Projekt: Tester czujników DS18B20 na ESP32 (standalone, z web UI)

## Cel
Firmware na ESP32, który służy jako warsztatowy tester kilkudziesięciu czujników
DS18B20 na jednej magistrali 1-Wire. Ma wykryć wszystkie czujniki, pokazać ich
pełne adresy ROM i temperatury na żywo na lokalnej stronie WWW, oraz wspierać
identyfikację „który ROM to który fizyczny czujnik" metodą ciepłej szklanki.
NIE integrować z Home Assistant / MQTT / ESPHome — ma być w pełni samodzielny.

## Kontekst sprzętowy (już ustalony, nie zmieniaj)
- Płytka: generyczny ESP32 DevKit (env esp32dev).
- Magistrala 1-Wire na GPIO4, jedna wspólna szyna.
- Pull-up 2.2 kΩ między DQ a 3.3 V.
- Zasilanie 3-żyłowe (VDD/GND/DQ), czujniki na 3.3 V — NIE parasite power.
- Kilkadziesiąt czujników (zakładaj do ~50) jednocześnie na magistrali.

## Stack
- PlatformIO, framework arduino.
- Biblioteki: paulstoffregen/OneWire, milesburton/DallasTemperature,
  bblanchon/ArduinoJson @ ^7 (użyj składni v7: JsonDocument, to<JsonArray>(),
  add<JsonObject>()), ESP32Async/AsyncTCP, ESP32Async/ESPAsyncWebServer @ ^3.
- Dostarcz platformio.ini + src/main.cpp + krótkie README (build + flash).

## Wymagania funkcjonalne
1. Skan magistrali przy starcie: wykryj wszystkie czujniki, policz je.
2. Dla każdego czujnika pokaż PEŁNY 8-bajtowy adres ROM:
   - w formacie czytelnym (16 znaków hex, wielkie litery, np. 28FF641E0016035C),
   - oraz w wariancie gotowym do wklejenia do ESPHome/HA (z prefiksem 0x).
   Zweryfikuj zgodność kolejności bajtów z tym, jak ESPHome zapisuje pole address.
3. Odczyt temperatury NIEBLOKUJĄCY:
   - setWaitForConversion(false),
   - jeden requestTemperatures() (Skip ROM + Convert T dla wszystkich naraz),
   - odczyt scratchpadów dopiero po czasie konwersji (750 ms dla 12-bit),
   - cały cykl na millis(), bez delay() w loop — web server ma być płynny.
4. WiFi w trybie Access Point (SSID/hasło w stałych na górze pliku), z opcją
   przełączenia na tryb STA (zakomentowane stałe).
5. Asynchroniczny web server (ESPAsyncWebServer) serwujący:
   - stronę HTML (w PROGMEM) z auto-odświeżaniem (fetch JSON co 1 s),
   - endpoint /data zwracający JSON z listą czujników,
   - endpoint /rescan do ponownego skanu magistrali.
6. Strona WWW:
   - tabela: ROM | temperatura | status,
   - WYRAŹNE podświetlenie wiersza czujnika z aktualnie NAJWYŻSZĄ temperaturą
     oraz pokazanie jego ROM na górze (to kluczowe dla metody ciepłej szklanki —
     wkładam czujniki po kolei do ciepłej wody i patrzę, który skoczył),
   - przycisk „Eksport CSV" (pobranie pliku),
   - przycisk „Kopiuj listę ROM" (do schowka),
   - przycisk „Skanuj ponownie" (wywołuje /rescan).
7. Detekcja błędów per czujnik i oznaczenie statusu:
   - 85.00 °C = brak udanej konwersji / słabe zasilanie (status reset85),
   - -127 °C = odłączony / brak komunikacji (status disconnected),
   - NaN / brak odczytu (status noread),
   - czujniki z błędem WYKLUCZ z wyliczania maksimum.
8. Flaga prawdopodobnego klona: ROM niezgodny ze wzorcem 28-xx-xx-xx-xx-00-00-xx
   oznacz wizualnie na stronie (nie odrzucaj, tylko zaznacz).

## Wymagania niefunkcjonalne
- Kod ma się kompilować pod PlatformIO bez ręcznych poprawek.
- Konfiguracja (pin 1-Wire, SSID/hasło, rozdzielczość, max liczba czujników)
  w stałych #define na górze pliku.
- Komentarze w kodzie po polsku.
- Rozdzielczość czujników ustawialna (domyślnie 12-bit).

## Czego NIE robić
- Bez integracji HA / MQTT / ESPHome / żadnej chmury.
- Bez localStorage/sessionStorage na stronie (trzymaj stan w JS w pamięci).
- Bez blokującego delay() w pętli głównej.

## Dodatkowo
Dodaj w README sekcję „dobór pull-up": jeśli przy skanie gubią się czujniki,
zejść z 2.2 kΩ na 1.5 kΩ, ewentualnie podzielić na dwie magistrale na osobnych
GPIO. Krótko, jedno-dwa zdania.
