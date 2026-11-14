# Astronaut Monitoring Device Firmware

Firmware for a local-network demo using an Arduino Uno R4 WiFi and an ESP32-CAM. The Uno reads pulse and ambient temperature/humidity sensors, provides an encoder-driven display workflow, and asks the camera to capture a JPEG. The camera uploads the photo directly to a separately hosted API.

This is a software/hardware demo, not a medical device. Do not use its readings for clinical or emergency decisions. The included device-to-server traffic uses plain HTTP and is suitable only for sample data on a trusted, isolated LAN.

## Contents

- `central-device-firmware/central_device.ino`: Uno R4 WiFi firmware.
- `camera-firmware/esp32_cam.ino`: ESP32-CAM firmware for the AI Thinker-style pinout.
- `docs/PINOUT.md`: Uno wiring and ESP32-CAM camera GPIO reference.
- `docs/API.md`: draft API contract and workflow notes.

The API server is not included in this repository. You must run a compatible backend separately; the firmware expects it on port `5000` and uses the `/api/v1` routes described in `docs/API.md`.

## Hardware and Software

- Arduino Uno R4 WiFi with the Arduino UNO R4 Boards package.
- AI Thinker ESP32-CAM (the camera GPIO mapping in the sketch is for this style of board) with Espressif's `esp32` boards package.
- Arduino IDE 2.x or Arduino CLI.
- Libraries: `ArduinoJson` and `Arduino_GFX_Library`. `WiFiS3` is provided by the Uno board package; `esp_camera` is provided by the ESP32 board package.
- A compatible backend reachable by both boards on the same LAN.

Install the libraries through Arduino IDE's Library Manager. Select the matching board for each sketch before compiling or uploading. The camera may require a USB-to-serial adapter and the board's boot-mode procedure; follow the board vendor's instructions.

## Configure

1. In `central-device-firmware`, copy `device_config.example.h` to `device_config.h`.
2. In `camera-firmware`, copy `device_config.example.h` to `device_config.h`.
3. Set the Wi-Fi network in both local files. Set `apiAddress` and `API_HOST` to the computer's LAN IP, and set `cameraAddress` to the ESP32-CAM's LAN IP.
4. Set the Uno and camera device IDs/keys and `CAMERA_ID` to values provisioned in your backend. The two sketches must use matching Wi-Fi, API, and pairing settings.
5. Keep each `device_config.h` local. Both files are ignored by Git; do not commit real Wi-Fi passwords, device keys, or tokens.

The default network values in the sketches are examples only. Do not use `localhost` as the API address: each board must connect to the computer's LAN address. Configure the backend to listen on the LAN interface and allow inbound TCP port `5000` through the computer's firewall. The camera also exposes its local capture-control service on port `80`; keep it on a trusted network.

If the Wi-Fi password previously used in these sketches was real, change it before using the network again. It was present in the firmware source and should be treated as disclosed.

## Pins

See [the pinout and wiring reference](docs/PINOUT.md) for all Uno connections, the TFT's hardware SPI pins, and the ESP32-CAM camera GPIO mapping. The camera is controlled over Wi-Fi and does not connect to the Uno's GPIO pins.

## Build and Upload

1. Start the compatible backend and confirm it is reachable from the boards at `http://<computer-lan-ip>:5000`.
2. Create and fill in both local `device_config.h` files as described above.
3. Open `central-device-firmware/central_device.ino`, select **Arduino Uno R4 WiFi**, then compile and upload.
4. Open `camera-firmware/esp32_cam.ino`, select the matching ESP32-CAM board, then compile and upload.
5. Open the serial monitor at `115200` baud for connection and upload diagnostics. The camera's flash LED is on GPIO4 and lights during capture. Its image is configured for QVGA (`320x240`) and rotated 180 degrees for an upside-down mount.

## Git Setup

The `.gitignore` excludes generated build output, editor/OS files, and the local device configuration headers. To initialize and stage this project:

```sh
git init
git add .
git status
```

Check `git status` before committing and verify that neither `device_config.h` appears in the staged files.