#include "esp_camera.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include "device_config.h"
#include <esp_log.h>
#include <esp_system.h>
#include <stdlib.h>
#include <string.h>

constexpr uint16_t FRAME_WIDTH = 320;
constexpr uint16_t FRAME_HEIGHT = 240;
constexpr uint8_t CAMERA_FLASH_LED_PIN = 4;
// Set to the local backend port; keep this in sync with the Uno sketch.
constexpr uint16_t API_SERVER_PORT = 5000;
constexpr int8_t CAMERA_PWDN = 32;
constexpr int8_t CAMERA_RESET = -1;
constexpr int8_t CAMERA_XCLK = 0;
constexpr int8_t CAMERA_SIOD = 26;
constexpr int8_t CAMERA_SIOC = 27;
constexpr int8_t CAMERA_Y9 = 35;
constexpr int8_t CAMERA_Y8 = 34;
constexpr int8_t CAMERA_Y7 = 39;
constexpr int8_t CAMERA_Y6 = 36;
constexpr int8_t CAMERA_Y5 = 21;
constexpr int8_t CAMERA_Y4 = 19;
constexpr int8_t CAMERA_Y3 = 18;
constexpr int8_t CAMERA_Y2 = 5;
constexpr int8_t CAMERA_VSYNC = 25;
constexpr int8_t CAMERA_HREF = 23;
constexpr int8_t CAMERA_PCLK = 22;

WiFiServer server(80);
String pendingMeasurementId;
String lastMeasurementId;
String lastCaptureStatus = "idle";

size_t readChunkedErrorBody(WiFiClient &client, char *body, size_t capacity, bool &truncated) {
  size_t received = 0;
  truncated = false;
  while (client.connected() || client.available()) {
    String sizeLine = client.readStringUntil('\n');
    sizeLine.trim();
    char *end = nullptr;
    unsigned long chunkLength = strtoul(sizeLine.c_str(), &end, 16);
    if (end == sizeLine.c_str() || chunkLength == 0) break;

    size_t remaining = static_cast<size_t>(chunkLength);
    while (remaining > 0) {
      size_t availableCapacity = capacity > received + 1 ? capacity - received - 1 : 0;
      if (availableCapacity == 0) truncated = true;
      uint8_t discard[32];
      size_t bytesToRead = remaining < sizeof(discard) ? remaining : sizeof(discard);
      if (availableCapacity > 0 && bytesToRead > availableCapacity) bytesToRead = availableCapacity;
      size_t bytesRead = availableCapacity > 0
                           ? client.readBytes(reinterpret_cast<uint8_t *>(body + received), bytesToRead)
                           : client.readBytes(discard, bytesToRead);
      if (bytesRead == 0) return received;
      if (availableCapacity > 0) received += bytesRead;
      remaining -= bytesRead;
    }
    if (client.read() != '\r' || client.read() != '\n') break;
  }
  if (capacity > 0) body[received] = '\0';
  return received;
}

bool startCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAMERA_Y2;
  config.pin_d1 = CAMERA_Y3;
  config.pin_d2 = CAMERA_Y4;
  config.pin_d3 = CAMERA_Y5;
  config.pin_d4 = CAMERA_Y6;
  config.pin_d5 = CAMERA_Y7;
  config.pin_d6 = CAMERA_Y8;
  config.pin_d7 = CAMERA_Y9;
  config.pin_xclk = CAMERA_XCLK;
  config.pin_pclk = CAMERA_PCLK;
  config.pin_vsync = CAMERA_VSYNC;
  config.pin_href = CAMERA_HREF;
  config.pin_sccb_sda = CAMERA_SIOD;
  config.pin_sccb_scl = CAMERA_SIOC;
  config.pin_pwdn = CAMERA_PWDN;
  config.pin_reset = CAMERA_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 12;
  bool hasPsram = psramFound();
  config.fb_count = 1;
  config.fb_location = hasPsram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_log_level_set("cam_hal", ESP_LOG_NONE);
  Serial.printf("[CAM DEBUG] Init: %ux%u JPEG, PSRAM=%s, frame buffers=1, grab=WHEN_EMPTY\n",
                FRAME_WIDTH, FRAME_HEIGHT, hasPsram ? "yes" : "no");
  esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK) {
    Serial.printf("[CAM ERROR] Camera init failed: 0x%x\n", result);
    return false;
  }
  esp_log_level_set("cam_hal", ESP_LOG_NONE);
  Serial.println("[CAM DEBUG] Camera initialized.");

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_framesize(sensor, FRAMESIZE_QVGA);
    sensor->set_hmirror(sensor, 1);
    sensor->set_vflip(sensor, 1);
    sensor->set_whitebal(sensor, 1);
    sensor->set_awb_gain(sensor, 1);
    sensor->set_exposure_ctrl(sensor, 1);
    sensor->set_gain_ctrl(sensor, 1);
  }
  return true;
}

bool uploadPhoto(const char *measurementId) {
  Serial.print("[CAM API] Capturing JPEG for measurement ");
  Serial.println(measurementId);
  digitalWrite(CAMERA_FLASH_LED_PIN, HIGH);
  delay(100);
  camera_fb_t *staleFrame = esp_camera_fb_get();
  if (staleFrame != nullptr) {
    esp_camera_fb_return(staleFrame);
  }
  camera_fb_t *frame = esp_camera_fb_get();
  digitalWrite(CAMERA_FLASH_LED_PIN, LOW);
  if (frame == nullptr || frame->format != PIXFORMAT_JPEG ||
      frame->width != FRAME_WIDTH || frame->height != FRAME_HEIGHT) {
    Serial.print("[CAM API] Invalid frame: ");
    if (frame == nullptr) {
      Serial.println("esp_camera_fb_get returned null.");
    } else {
      Serial.print("format=");
      Serial.print(static_cast<int>(frame->format));
      Serial.print(" size=");
      Serial.print(frame->width);
      Serial.print('x');
      Serial.println(frame->height);
    }
    if (frame != nullptr) {
      esp_camera_fb_return(frame);
    }
    return false;
  }

  Serial.print("[CAM API] Frame ready: ");
  Serial.print(frame->width);
  Serial.print('x');
  Serial.print(frame->height);
  Serial.print(", ");
  Serial.print(frame->len);
  Serial.println(" JPEG bytes.");

  WiFiClient client;
  client.setTimeout(20000);
  if (!client.connect(API_HOST, API_SERVER_PORT)) {
    Serial.print("[CAM API] TCP connect failed to ");
    Serial.print(API_HOST);
    Serial.print(':');
    Serial.print(API_SERVER_PORT);
    Serial.print("; WiFi status=");
    Serial.println(static_cast<int>(WiFi.status()));
    esp_camera_fb_return(frame);
    return false;
  }

  Serial.println("[CAM API] TCP connected; sending photo request over plain HTTP.");
  char path[128];
  char idempotencyKey[40];
  snprintf(path, sizeof(path), "/api/v1/measurements/%s/photo", measurementId);
  snprintf(idempotencyKey, sizeof(idempotencyKey), "%08lx-%04x-4%03x-%04x-%012lx",
           static_cast<unsigned long>(millis()),
           static_cast<unsigned int>(esp_random() & 0xffff),
           static_cast<unsigned int>(esp_random() & 0x0fff),
           static_cast<unsigned int>(esp_random() & 0xffff),
           static_cast<unsigned long>(esp_random()));
  client.print("PUT ");
  client.print(path);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(API_HOST);
  client.print("\r\nContent-Type: image/jpeg\r\nX-Device-Key: ");
  client.print(CAMERA_DEVICE_KEY);
  client.print("\r\nIdempotency-Key: ");
  client.print(idempotencyKey);
  client.print("\r\nConnection: close\r\nContent-Length: ");
  client.print(frame->len);
  client.print("\r\n\r\n");

  size_t sent = 0;
  uint32_t uploadStartedMs = millis();
  while (sent < frame->len) {
    size_t written = client.write(frame->buf + sent, frame->len - sent);
    if (written == 0) {
      Serial.print("[CAM API] Upload stopped while sending image; sent ");
      Serial.print(sent);
      Serial.print(" of ");
      Serial.print(frame->len);
      Serial.println(" bytes.");
      client.stop();
      esp_camera_fb_return(frame);
      return false;
    }
    sent += written;
  }

  Serial.print("[CAM API] JPEG send complete: ");
  Serial.print(sent);
  Serial.print(" bytes in ");
  Serial.print(millis() - uploadStartedMs);
  Serial.println(" ms; waiting for server response.");

  String statusLine = client.readStringUntil('\n');
  int firstSpace = statusLine.indexOf(' ');
  int statusCode = firstSpace >= 0 ? statusLine.substring(firstSpace + 1, firstSpace + 4).toInt() : 0;
  bool uploaded = statusCode >= 200 && statusCode < 300;
  int responseLength = 0;
  bool chunkedResponse = false;
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
    if (line.startsWith("Content-Length:")) {
      responseLength = line.substring(15).toInt();
    } else {
      String normalizedLine = line;
      normalizedLine.toLowerCase();
      if (normalizedLine.startsWith("transfer-encoding:") && normalizedLine.indexOf("chunked") >= 0) {
        chunkedResponse = true;
      }
    }
  }
  if (uploaded) {
    Serial.print("[CAM API] Upload accepted, HTTP ");
    Serial.print(statusCode);
    Serial.print("; JPEG bytes=");
    Serial.println(frame->len);
  } else {
    if (statusCode == 0) {
      Serial.println("[CAM API] No HTTP status line received. Verify host/port and whether the server expects HTTPS; this client sends plain HTTP.");
    } else {
      Serial.print("[CAM API] Upload rejected; HTTP ");
      Serial.print(statusCode);
      Serial.print(": ");
      Serial.println(statusLine);
    }
    Serial.print("[CAM API] Response headers: Content-Length=");
    Serial.print(responseLength);
    Serial.print(", chunked=");
    Serial.println(chunkedResponse ? "yes" : "no");
    if (responseLength > 0) {
      char errorBody[161] = {};
      size_t bytesToRead = responseLength < static_cast<int>(sizeof(errorBody) - 1)
                             ? static_cast<size_t>(responseLength)
                             : sizeof(errorBody) - 1;
      size_t bytesRead = client.readBytes(errorBody, bytesToRead);
      Serial.print("[CAM API] Error body: ");
      Serial.write(reinterpret_cast<const uint8_t *>(errorBody), bytesRead);
      Serial.println();
    } else if (chunkedResponse) {
      char errorBody[161] = {};
      bool truncated = false;
      size_t bytesRead = readChunkedErrorBody(client, errorBody, sizeof(errorBody), truncated);
      Serial.print("[CAM API] Error body: ");
      Serial.write(reinterpret_cast<const uint8_t *>(errorBody), bytesRead);
      if (truncated) Serial.print("...");
      Serial.println();
    }
  }
  client.stop();
  esp_camera_fb_return(frame);
  return uploaded;
}

void sendJson(WiFiClient &client, int statusCode, const char *reason, const String &body) {
  client.print("HTTP/1.1 ");
  client.print(statusCode);
  client.print(' ');
  client.print(reason);
  client.print("\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ");
  client.print(body.length());
  client.print("\r\n\r\n");
  client.print(body);
}

bool validMeasurementId(const char *measurementId) {
  if (measurementId == nullptr || strlen(measurementId) < 8 || strlen(measurementId) > 40) return false;
  for (const char *character = measurementId; *character != '\0'; character++) {
    if (!((*character >= 'a' && *character <= 'z') || (*character >= '0' && *character <= '9') || *character == '_' || *character == '-')) {
      return false;
    }
  }
  return true;
}

void servicePendingCapture() {
  if (pendingMeasurementId.length() == 0) return;
  String measurementId = pendingMeasurementId;
  pendingMeasurementId = "";
  lastCaptureStatus = "capturing";
  bool uploaded = uploadPhoto(measurementId.c_str());
  lastCaptureStatus = uploaded ? "uploaded" : "failed";
  Serial.print("[CAM DEBUG] Upload result=");
  Serial.print(lastCaptureStatus);
  Serial.print(" measurement=");
  Serial.println(measurementId);
}

void setup() {
  Serial.begin(115200);
  pinMode(CAMERA_FLASH_LED_PIN, OUTPUT);
  digitalWrite(CAMERA_FLASH_LED_PIN, LOW);

  if (!startCamera()) {
    while (true) {
      delay(1000);
    }
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[CAM DEBUG] Connecting to WiFi.");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  server.begin();
  Serial.print("[CAM DEBUG] WiFi connected; camera IP=");
  Serial.print(WiFi.localIP());
  Serial.print(" RSSI=");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
  Serial.print("[CAM DEBUG] API target=http://");
  Serial.print(API_HOST);
  Serial.print(':');
  Serial.print(API_SERVER_PORT);
  Serial.println("/api/v1; capture control=HTTP :80");
}

void loop() {
  WiFiClient client = server.available();
  if (!client) {
    return;
  }

  client.setTimeout(3000);
  String requestLine = client.readStringUntil('\n');
  requestLine.trim();
  int contentLength = 0;
  while (client.connected()) {
    String header = client.readStringUntil('\n');
    if (header.length() <= 1) {
      break;
    }
    if (header.startsWith("Content-Length:")) {
      contentLength = header.substring(15).toInt();
    }
  }

  if (requestLine.startsWith("POST /capture ")) {
    Serial.print("[CAM DEBUG] Capture request received; body bytes=");
    Serial.println(contentLength);
    if (contentLength <= 0 || contentLength > 256) {
      sendJson(client, 400, "Bad Request", "{\"status\":\"invalid_request\"}");
    } else {
      char body[257] = {};
      size_t received = client.readBytes(body, contentLength);
      StaticJsonDocument<256> document;
      if (received != static_cast<size_t>(contentLength) || deserializeJson(document, body)) {
        sendJson(client, 400, "Bad Request", "{\"status\":\"invalid_json\"}");
      } else {
        const char *measurementId = document["measurement_id"] | "";
        if (!validMeasurementId(measurementId) || pendingMeasurementId.length() > 0) {
          sendJson(client, 409, "Conflict", "{\"status\":\"invalid_or_busy\"}");
        } else {
          pendingMeasurementId = measurementId;
          lastMeasurementId = measurementId;
          lastCaptureStatus = "queued";
          Serial.print("[CAM DEBUG] Capture queued; measurement=");
          Serial.println(measurementId);
          sendJson(client, 202, "Accepted", "{\"status\":\"capture_queued\"}");
        }
      }
    }
  } else if (requestLine.startsWith("GET /capture/") && requestLine.indexOf("/status ") > 0) {
    int start = strlen("GET /capture/");
    int end = requestLine.indexOf("/status ");
    String requestedId = requestLine.substring(start, end);
    if (requestedId == lastMeasurementId) {
      String body = "{\"measurement_id\":\"" + lastMeasurementId + "\",\"status\":\"" + lastCaptureStatus + "\"}";
      sendJson(client, 200, "OK", body);
    } else {
      sendJson(client, 404, "Not Found", "{\"status\":\"unknown_measurement\"}");
    }
  } else if (requestLine.startsWith("GET /capture ")) {
    sendJson(client, 405, "Method Not Allowed", "{\"error\":\"image_return_disabled\"}");
  } else {
    sendJson(client, 200, "OK", "{\"status\":\"camera_ready\"}");
  }
  client.stop();
  servicePendingCapture();
}