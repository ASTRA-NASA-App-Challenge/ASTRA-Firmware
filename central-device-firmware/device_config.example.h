#pragma once

const char WIFI_SSID[] = "YOUR_WIFI_SSID";
const char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";
const char UNO_DEVICE_ID[] = "uno-demo-01";
const char UNO_DEVICE_KEY[] = "REPLACE_WITH_UNO_DEVICE_KEY";
const char CAMERA_ID[] = "cam-demo-01";

// Set these to the computer running the API and this camera's LAN addresses.
IPAddress apiAddress(192, 168, 1, 10);
IPAddress cameraAddress(192, 168, 1, 11);