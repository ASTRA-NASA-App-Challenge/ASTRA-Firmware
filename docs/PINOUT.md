# Device Pinout

Pin assignments in this document match the current firmware. Check the exact module variant and its voltage requirements before wiring; breakout-board labels and power requirements can differ.

## Arduino Uno R4 WiFi

### GC9A01 TFT

The sketch creates an `Arduino_HWSPI` bus, so clock and MOSI use the Uno's hardware SPI pins.

| TFT signal | Uno R4 pin | Firmware setting |
| --- | --- | --- |
| CS | D10 | `TFT_CS` |
| DC | D7 | `TFT_DC` |
| RESET | D8 | `TFT_RST` |
| SCK / CLK | D13 / SPI SCK | Hardware SPI |
| SDA / MOSI | D11 / SPI MOSI | Hardware SPI |
| SDO / MISO | Not connected | Display is used as write-only |
| VCC, GND | Per display module | Check the module's voltage requirements |

D12 is the Uno's SPI MISO pin and is also assigned to the buzzer. Do not connect a TFT MISO/SDO wire. If display reads are enabled later, move the buzzer to a free pin and update the firmware.

### Sensors and Controls

| Module signal | Uno R4 pin | Notes |
| --- | --- | --- |
| Pulse sensor analog output | A0 | Keep its output within the Uno's supported analog input range. |
| DHT11 DATA | D4 | Use the pull-up required by the sensor/module. |
| Encoder CLK | D2 | Configured with internal pull-up; interrupt-driven. |
| Encoder DT | D3 | Configured with internal pull-up; interrupt-driven. |
| Encoder switch | D5 | Configured with internal pull-up; connect switch to GND when pressed. |
| Buzzer signal | D12 | Use a transistor driver for loads that exceed safe GPIO current. |

Connect module grounds to Uno GND. Connect power only according to each module's requirements; do not assume every breakout accepts 5 V logic or power.

## ESP32-CAM

The camera pin assignments below are the AI Thinker-style mapping configured in `camera-firmware/esp32_cam.ino`.

| Camera signal | ESP32 GPIO |
| --- | ---: |
| PWDN | 32 |
| RESET | Not connected (`-1`) |
| XCLK | 0 |
| SIOD / SDA | 26 |
| SIOC / SCL | 27 |
| Y2 / D0 | 5 |
| Y3 / D1 | 18 |
| Y4 / D2 | 19 |
| Y5 / D3 | 21 |
| Y6 / D4 | 36 |
| Y7 / D5 | 39 |
| Y8 / D6 | 34 |
| Y9 / D7 | 35 |
| VSYNC | 25 |
| HREF | 23 |
| PCLK | 22 |
| Flash LED | 4 |

The camera and flash wiring are normally handled by the ESP32-CAM board itself; these GPIOs are listed for board identification and troubleshooting, not for external wiring. The flash LED on GPIO4 turns on during capture. Power the ESP32-CAM using the board vendor's recommended supply; do not power it from an Uno GPIO pin.

## Board-to-Board Link

There is no direct GPIO connection between the Uno and ESP32-CAM. They join the same Wi-Fi network. The Uno sends a capture request to the camera's configured LAN address on port `80`; the camera sends JPEG data directly to the backend on port `5000`.