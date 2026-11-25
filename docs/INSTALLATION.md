# Installation and compilation

## 1. Install Arduino IDE

Download from https://www.arduino.cc/en/software. In Preferences, add the ESP32 board manager URL if needed:

`https://espressif.github.io/arduino-esp32/package_esp32_index.json`

In Boards Manager, install **esp32 by Espressif Systems**. Select **ESP32 Dev Module**, not ESP32-S3. Set the flash size to match your board (commonly 4 MB), and select its serial port. Start with an upload speed of 115200 if unsure.

## 2. Install libraries

In Library Manager install TFT_eSPI by Bodmer, ArduinoJson 7 by Benoit Blanchon (the sketch uses `JsonDocument`), and XPT2046_Touchscreen by Paul Stoffregen. WiFi, HTTPClient, WebServer, DNSServer, Preferences and SPI are provided by the ESP32 core. Exact tested library/core versions were not recorded in this package.

## 3. Configure the display library

Copy `TFT_eSPI/Setup_CYD_METAR.h` from this repository into the installed TFT_eSPI library's `User_Setups` folder. Open the library's `User_Setup_Select.h`, comment out `#include <User_Setup.h>` and enable only:

```cpp
#include <User_Setups/Setup_CYD_METAR.h>
```

Back up your previous configuration. Library updates can overwrite this change. The provided header is reconstructed from the project's known driver/color settings and published CYD pin assignments; confirm it on your hardware. See https://github.com/Bodmer/TFT_eSPI/discussions/3943 and https://randomnerdtutorials.com/esp32-cheap-yellow-display-cyd-pinout-esp32-2432s028r/.

The display uses HSPI; touchscreen uses separate VSPI pins defined in the sketch. The sketch initializes the display in the order `init()`, `setRotation(0)`, `invertDisplay(true)`. Do not enable another TFT driver simultaneously.

## 4. Compile and upload

Open `firmware/METAR_Mini/METAR_Mini.ino`. Connect the board with a USB data cable. Click Verify, then Upload. Open Serial Monitor at 115200 baud for diagnostics. If upload fails, try another cable/port, lower upload speed, or hold BOOT while the IDE connects and release when upload begins.

## 5. First boot

Connect to the setup access point, configure Wi-Fi and airports, then save. Allow time for weather and runway downloads. Use 2.4 GHz Wi-Fi with internet access. No external sensor is required.

## Troubleshooting

- White/incorrect colors: check active TFT setup and the actual LCD controller revision; ST7789 variants are not covered by the supplied configuration.
- Touch not responding: check XPT2046 library and the touch pin definitions; panel calibration may vary.
- Setup page unavailable: stay connected to METAR-Setup even if the phone reports no internet; open the HTTP address manually.
- No weather: check internet, airport identifier, reporting availability and serial diagnostics.
- No runway: allow database retrieval; use manual mode when data is unavailable.
- Power issues: use a suitable USB supply/cable; some CYD USB-C revisions require USB-A to USB-C.
