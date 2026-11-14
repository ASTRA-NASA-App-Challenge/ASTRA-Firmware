#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <WiFiS3.h>
#include "device_config.h"
#include <string.h>

#define TFT_CS   10
#define TFT_DC   7
#define TFT_RST  8

#define BLACK   RGB565_BLACK
#define RED     RGB565_RED
#define GREEN   RGB565_GREEN
#define BLUE    RGB565_BLUE
#define WHITE   RGB565_WHITE
#define CYAN    RGB565_CYAN
#define YELLOW  RGB565_YELLOW

constexpr int16_t SCREEN_CENTER = 120;
constexpr uint8_t HEART_RATE_PIN = A0;
constexpr uint8_t DHT_PIN = 4;
constexpr uint8_t ENCODER_CLK_PIN = 2;
constexpr uint8_t ENCODER_DT_PIN = 3;
constexpr uint8_t ENCODER_SW_PIN = 5;
constexpr uint8_t BUZZER_PIN = 12;
constexpr uint16_t CAMERA_FRAME_WIDTH = 320;
constexpr uint16_t CAMERA_FRAME_HEIGHT = 240;
constexpr uint32_t CAMERA_READ_TIMEOUT_MS = 8000;
constexpr uint32_t DHT_READ_INTERVAL_MS = 2000;
constexpr uint32_t ENVIRONMENT_POST_INTERVAL_MS = 30000;
// Set to the local backend port; keep this in sync with the camera sketch.
constexpr uint16_t API_SERVER_PORT = 5000;
constexpr uint16_t CAMERA_PORT = 80;
constexpr int8_t SENSOR_POLARITY = 1;
constexpr uint32_t SAMPLE_INTERVAL_MS = 5;
constexpr uint8_t INTERVAL_HISTORY_SIZE = 5;
constexpr uint32_t MIN_BEAT_INTERVAL_MS = 300;
constexpr uint32_t MAX_BEAT_INTERVAL_MS = 2000;
constexpr uint32_t MIN_PULSE_WIDTH_MS = 25;
constexpr uint32_t MAX_PULSE_WIDTH_MS = 600;
constexpr uint8_t MIN_INTERVALS_FOR_READING = 3;
constexpr uint16_t MIN_DISPLAY_BPM = 40;
constexpr uint16_t MAX_DISPLAY_BPM = 180;

enum WorkflowState : uint8_t {
  WAITING_FOR_ROSTER,
  SELECTING_ASTRONAUT,
  ENTERING_PIN,
  RECORDING_PULSE,
  TAKING_PHOTO,
  WAITING_FOR_UPLOAD,
  SESSION_COMPLETE
};

constexpr uint8_t PIN_BACKSPACE_OPTION = 10;
constexpr uint8_t PIN_LOGIN_OPTION = 11;
constexpr uint8_t PIN_CANCEL_OPTION = 12;
constexpr uint8_t PIN_OPTION_COUNT = 13;
constexpr uint8_t PIN_LENGTH = 6;
constexpr uint32_t TOKEN_REFRESH_MARGIN_MS = 180000;
constexpr uint32_t PHOTO_UPLOAD_TIMEOUT_MS = 20000;
constexpr uint32_t COMPLETE_SCREEN_TIME_MS = 2500;

Arduino_DataBus *bus = new Arduino_HWSPI(TFT_DC, TFT_CS);

Arduino_GFX *gfx = new Arduino_GC9A01(
  bus,
  TFT_RST,
  0,
  true
);

float signalBaseline = 0.0f;
float filteredPulse = 0.0f;
float pulseEnvelope = 0.0f;
bool filterReady = false;
bool pulseActive = false;
uint32_t lastSampleMs = 0;
uint32_t lastBeatMs = 0;
uint32_t beatIntervals[INTERVAL_HISTORY_SIZE] = {};
uint8_t beatIntervalCount = 0;
uint8_t nextIntervalIndex = 0;
float activePulseThreshold = 0.0f;
uint32_t pulseStartMs = 0;
int heartRateBpm = 0;
uint32_t lastDhtReadMs = 0;
int temperatureC = 0;
int humidityPercent = 0;
bool temperatureValid = false;
uint8_t dhtErrorCode = 0;
uint32_t lastWifiAttemptMs = 0;
bool wifiWasConnected = false;
WorkflowState workflowState = WAITING_FOR_ROSTER;
uint8_t selectedAstronautIndex = 0;
uint8_t selectedPinOption = 0;
char pinDigits[PIN_LENGTH + 1] = {};
uint8_t pinLength = 0;
int recordedBpm = 0;
uint32_t completionScreenStartedMs = 0;
char activeMeasurementId[64] = {};
uint32_t lastRosterFetchAttemptMs = 0;
char workflowNotice[32] = {};
volatile int16_t encoderTransitions = 0;
volatile uint8_t encoderPreviousState = 0;
int16_t encoderTransitionRemainder = 0;
bool previousButtonReading = HIGH;
bool stableButtonState = HIGH;
uint32_t buttonChangeMs = 0;
uint32_t lastEnvironmentPostMs = 0;
uint32_t accessTokenExpiresAt = 0;
char accessToken[160] = {};
char refreshToken[160] = {};
char astronautId[48] = {};
char astronautUsername[40] = {};
bool rosterLoaded = false;

struct AstronautOption {
  char id[48];
  char username[40];
  char displayName[64];
};

AstronautOption astronautOptions[8] = {};
uint8_t astronautCount = 0;

const int8_t ENCODER_TRANSITION_TABLE[16] = {
  0, -1, 1, 0,
  1, 0, 0, -1,
  -1, 0, 0, 1,
  0, 1, -1, 0
};

void playErrorTone() {
  tone(BUZZER_PIN, 320, 180);
}

void playUiTone(uint16_t frequency, uint16_t durationMs = 35) {
  tone(BUZZER_PIN, frequency, durationMs);
}

void drawBpm(int bpm) {
  if (workflowState != RECORDING_PULSE) {
    return;
  }
  gfx->fillRect(50, 88, 140, 54, BLACK);
  gfx->setTextSize(5);
  gfx->setTextColor(bpm > 0 ? CYAN : WHITE);

  if (bpm > 0) {
    int digits = bpm < 10 ? 1 : (bpm < 100 ? 2 : 3);
    gfx->setCursor(SCREEN_CENTER - (digits * 30) / 2, 92);
    gfx->print(bpm);
  } else {
    gfx->setCursor(SCREEN_CENTER - 30, 92);
    gfx->print("--");
  }
}

void drawStatus(bool hasPulse) {
  if (workflowState != RECORDING_PULSE) {
    return;
  }
  gfx->fillRect(45, 172, 150, 20, BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(hasPulse ? GREEN : YELLOW);
  gfx->setCursor(hasPulse ? 93 : 84, 178);
  gfx->print(hasPulse ? "ACQUIRING" : "CHECK SENSOR");
}

void drawReadingStatus() {
  if (workflowState != RECORDING_PULSE) {
    return;
  }
  gfx->fillRect(45, 172, 150, 20, BLACK);
  gfx->setTextSize(1);

  if (heartRateBpm < MIN_DISPLAY_BPM || heartRateBpm > MAX_DISPLAY_BPM ||
      lastBeatMs == 0 || millis() - lastBeatMs > 3000) {
    gfx->setTextColor(YELLOW);
    gfx->setCursor(82, 178);
    gfx->print("CHECK READING");
  } else {
    gfx->setTextColor(GREEN);
    gfx->setCursor(98, 178);
    gfx->print("RATE OK");
  }
}

uint8_t readEncoderState() {
  return (digitalRead(ENCODER_CLK_PIN) << 1) | digitalRead(ENCODER_DT_PIN);
}

void onEncoderChange() {
  uint8_t currentState = readEncoderState();
  uint8_t transition = (encoderPreviousState << 2) | currentState;
  encoderTransitions += ENCODER_TRANSITION_TABLE[transition];
  encoderPreviousState = currentState;
}

void drawWorkflowScreen() {
  gfx->fillScreen(BLACK);
  gfx->drawCircle(SCREEN_CENTER, SCREEN_CENTER, 112, CYAN);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  if (workflowState == WAITING_FOR_ROSTER) {
    gfx->setCursor(42, 102);
    gfx->print("LOADING CREW");
  } else if (workflowState == SELECTING_ASTRONAUT) {
    gfx->setCursor(38, 48);
    gfx->print("SELECT ASTRONAUT");
    if (astronautCount > 0) {
      const AstronautOption &selected = astronautOptions[selectedAstronautIndex];
      int16_t nameX = SCREEN_CENTER - static_cast<int16_t>(strlen(selected.displayName) * 6);
      gfx->setCursor(nameX < 12 ? 12 : nameX, 96);
      gfx->print(selected.displayName);
      gfx->setTextSize(1);
      gfx->setCursor(70, 126);
      gfx->print(selected.username);
      gfx->setCursor(90, 151);
      gfx->print(selectedAstronautIndex + 1);
      gfx->print(" / ");
      gfx->print(astronautCount);
      gfx->setCursor(67, 181);
      gfx->print("TURN / PRESS");
    } else {
      gfx->setCursor(50, 110);
      gfx->print("NO CREW FOUND");
    }
  } else if (workflowState == ENTERING_PIN) {
    gfx->setTextSize(1);
    gfx->setCursor(75, 50);
    gfx->print("PIN FOR ");
    gfx->print(astronautOptions[selectedAstronautIndex].displayName);
    gfx->setTextSize(2);
    gfx->setCursor(SCREEN_CENTER - pinLength * 6, 86);
    for (uint8_t index = 0; index < pinLength; index++) gfx->print('*');
    gfx->setTextColor(CYAN);
    gfx->setTextSize(2);
    gfx->setCursor(92, 130);
    if (selectedPinOption <= 9) gfx->print(selectedPinOption);
    else if (selectedPinOption == PIN_BACKSPACE_OPTION) gfx->print("BK");
    else if (selectedPinOption == PIN_LOGIN_OPTION) gfx->print("OK");
    else gfx->print("CANCEL");
    gfx->setTextColor(WHITE);
    gfx->setTextSize(1);
    gfx->setCursor(49, 177);
    gfx->print("TURN: OPTION  PRESS: USE");
    gfx->setCursor(86, 214);
    gfx->print(pinLength);
    gfx->print(" / ");
    gfx->print(PIN_LENGTH);
    if (workflowNotice[0] != '\0') {
      gfx->setTextColor(YELLOW);
      gfx->setCursor(78, 198);
      gfx->print(workflowNotice);
      gfx->setTextColor(WHITE);
    }
  } else if (workflowState == RECORDING_PULSE) {
    gfx->setCursor(53, 50);
    gfx->print("RECORD BPM");
    gfx->setTextSize(1);
    gfx->setCursor(67, 75);
    gfx->print(astronautUsername);
    drawBpm(heartRateBpm);
    gfx->setTextSize(2);
    gfx->setCursor(102, 147);
    gfx->print("BPM");
    if (heartRateBpm > 0) drawReadingStatus();
    else drawStatus(lastBeatMs != 0 && millis() - lastBeatMs <= 3000);
    gfx->setTextSize(1);
    gfx->setCursor(61, 198);
    if (workflowNotice[0] != '\0') {
      gfx->setTextColor(YELLOW);
      gfx->print(workflowNotice);
      gfx->setTextColor(WHITE);
    } else {
      gfx->print("PRESS TO SAVE READING");
    }
  } else if (workflowState == TAKING_PHOTO) {
    gfx->setCursor(73, 52);
    gfx->print("BPM SAVED");
    gfx->setTextSize(4);
    gfx->setTextColor(CYAN);
    gfx->setCursor(recordedBpm < 100 ? 92 : 75, 91);
    gfx->print(recordedBpm);
    gfx->setTextColor(WHITE);
    gfx->setTextSize(1);
    gfx->setCursor(57, 162);
    gfx->print("PRESS TO TAKE PHOTO");
    if (workflowNotice[0] != '\0') {
      gfx->setTextColor(YELLOW);
      gfx->setCursor(42, 190);
      gfx->print(workflowNotice);
      gfx->setTextColor(WHITE);
    }
  } else if (workflowState == WAITING_FOR_UPLOAD) {
    gfx->setCursor(69, 96);
    gfx->print("UPLOADING");
    gfx->setTextSize(1);
    gfx->setCursor(70, 132);
    gfx->print("PLEASE WAIT...");
  } else {
    gfx->setCursor(82, 91);
    gfx->print("COMPLETE");
    gfx->setTextSize(1);
    gfx->setCursor(71, 126);
    gfx->print("LOGGED OUT");
  }
}

void drawCameraPrompt(const char *message) {
  gfx->fillScreen(BLACK);
  gfx->drawCircle(SCREEN_CENTER, SCREEN_CENTER, 112, CYAN);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(78, 60);
  gfx->print("CAMERA");
  gfx->setTextSize(1);
  gfx->setCursor(64, 105);
  gfx->print("PRESS TO SEND PHOTO");
  gfx->setTextColor(YELLOW);
  gfx->setCursor(80, 178);
  gfx->print(message);
}

void logApiFailure(const char *method, const char *path, const char *reason) {
  Serial.print("[API] ");
  Serial.print(method);
  Serial.print(' ');
  Serial.print(path);
  Serial.print(" failed: ");
  Serial.println(reason);
}

bool readChunkedResponse(WiFiClient &client, char *responseBody, size_t responseCapacity, size_t &received) {
  received = 0;
  while (client.connected() || client.available()) {
    String chunkLine = client.readStringUntil('\n');
    chunkLine.trim();
    if (chunkLine.length() == 0) return false;

    size_t chunkLength = 0;
    bool hasChunkLength = false;
    for (size_t index = 0; index < chunkLine.length(); index++) {
      char character = chunkLine[index];
      if (character == ';') break;
      uint8_t digit;
      if (character >= '0' && character <= '9') digit = character - '0';
      else if (character >= 'a' && character <= 'f') digit = character - 'a' + 10;
      else if (character >= 'A' && character <= 'F') digit = character - 'A' + 10;
      else return false;
      if (chunkLength > (static_cast<size_t>(-1) - digit) / 16) return false;
      chunkLength = chunkLength * 16 + digit;
      hasChunkLength = true;
    }
    if (!hasChunkLength) return false;

    if (chunkLength == 0) {
      while (client.connected() || client.available()) {
        String trailer = client.readStringUntil('\n');
        if (trailer.length() == 0 || trailer == "\r") return true;
      }
      return false;
    }
    if (chunkLength >= responseCapacity - received) return false;

    size_t chunkBytesRead = client.readBytes(responseBody + received, chunkLength);
    if (chunkBytesRead != chunkLength) return false;
    received += chunkBytesRead;
    if (client.read() != '\r' || client.read() != '\n') return false;
  }
  return false;
}

bool apiRequest(const char *method, const char *path, const char *contentType,
                const uint8_t *payload, size_t payloadLength, const char *token,
                const char *idempotencyKey, bool useDeviceKey, char *responseBody,
                size_t responseCapacity, int &statusCode) {
  WiFiClient client;
  client.setTimeout(CAMERA_READ_TIMEOUT_MS);
  if (!client.connect(apiAddress, API_SERVER_PORT)) {
    Serial.print("[API] TCP connect failed to ");
    Serial.print(apiAddress);
    Serial.print(':');
    Serial.print(API_SERVER_PORT);
    Serial.print(" for ");
    Serial.print(path);
    Serial.print("; WiFi status=");
    Serial.println(WiFi.status());
    Serial.print("[API] Uno IP=");
    Serial.print(WiFi.localIP());
    Serial.print(" gateway=");
    Serial.print(WiFi.gatewayIP());
    Serial.print(" subnet=");
    Serial.print(WiFi.subnetMask());
    Serial.print(" RSSI=");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    return false;
  }

  client.print(method);
  client.print(' ');
  client.print(path);
  client.print(" HTTP/1.1\r\nHost: ");
  client.print(apiAddress);
  client.print("\r\nConnection: close\r\n");
  if (contentType != nullptr) {
    client.print("Content-Type: ");
    client.print(contentType);
    client.print("\r\n");
  }
  if (useDeviceKey) {
    client.print("X-Device-Id: ");
    client.print(UNO_DEVICE_ID);
    client.print("\r\n");
    client.print("X-Device-Key: ");
    client.print(UNO_DEVICE_KEY);
    client.print("\r\n");
  }
  if (token != nullptr) {
    client.print("Authorization: Bearer ");
    client.print(token);
    client.print("\r\n");
  }
  if (idempotencyKey != nullptr) {
    client.print("Idempotency-Key: ");
    client.print(idempotencyKey);
    client.print("\r\n");
  }
  client.print("Content-Length: ");
  client.print(static_cast<unsigned long>(payloadLength));
  client.print("\r\n\r\n");
  if (payloadLength > 0 && client.write(payload, payloadLength) != payloadLength) {
    logApiFailure(method, path, "request body write failed");
    client.stop();
    return false;
  }

  String statusLine = client.readStringUntil('\n');
  int firstSpace = statusLine.indexOf(' ');
  statusCode = firstSpace >= 0 ? statusLine.substring(firstSpace + 1, firstSpace + 4).toInt() : 0;
  if (statusCode == 0) {
    logApiFailure(method, path, "invalid or missing HTTP status line");
    Serial.print("[API] status line: ");
    Serial.println(statusLine);
    client.stop();
    return false;
  }
  int contentLength = 0;
  bool chunkedResponse = false;
  while (client.connected() || client.available()) {
    String header = client.readStringUntil('\n');
    if (header == "\r" || header.length() == 0) {
      break;
    }
    String normalizedHeader = header;
    normalizedHeader.toLowerCase();
    if (normalizedHeader.startsWith("content-length:")) {
      contentLength = header.substring(15).toInt();
    } else if (normalizedHeader.startsWith("transfer-encoding:") &&
               normalizedHeader.indexOf("chunked") >= 0) {
      chunkedResponse = true;
    }
  }

  if (!chunkedResponse && (contentLength < 0 || static_cast<size_t>(contentLength) >= responseCapacity)) {
    logApiFailure(method, path, "invalid response length or response buffer too small");
    Serial.print("[API] HTTP ");
    Serial.print(statusCode);
    Serial.print(", Content-Length ");
    Serial.print(contentLength);
    Serial.print(", buffer capacity ");
    Serial.println(responseCapacity);
    client.stop();
    return false;
  }
  size_t received = 0;
  if (chunkedResponse) {
    if (!readChunkedResponse(client, responseBody, responseCapacity, received)) {
      logApiFailure(method, path, "invalid, oversized, or incomplete chunked response");
      Serial.print("[API] decoded response bytes before failure: ");
      Serial.println(received);
      client.stop();
      return false;
    }
  } else {
    uint32_t startMs = millis();
    while (received < static_cast<size_t>(contentLength) && millis() - startMs < CAMERA_READ_TIMEOUT_MS) {
      if (client.available()) {
        responseBody[received++] = static_cast<char>(client.read());
      } else if (!client.connected()) {
        break;
      } else {
        delay(1);
      }
    }
  }
  client.stop();
  responseBody[received] = '\0';
  if (!chunkedResponse && received != static_cast<size_t>(contentLength)) {
    logApiFailure(method, path, "response body was incomplete or timed out");
    Serial.print("[API] received ");
    Serial.print(received);
    Serial.print(" of ");
    Serial.print(contentLength);
    Serial.println(" response bytes");
    return false;
  }
  if (statusCode < 200 || statusCode >= 300) {
    Serial.print("[API] ");
    Serial.print(method);
    Serial.print(' ');
    Serial.print(path);
    Serial.print(" returned HTTP ");
    Serial.println(statusCode);
    Serial.print("[API] error body: ");
    for (size_t index = 0; index < received && index < 160; index++) {
      Serial.print(responseBody[index]);
    }
    Serial.println();
  }
  return true;
}

void copyText(char *destination, size_t capacity, const char *source) {
  if (capacity == 0) return;
  if (source == nullptr) source = "";
  strncpy(destination, source, capacity - 1);
  destination[capacity - 1] = '\0';
}

void makeRequestId(char *output, size_t capacity) {
  snprintf(output, capacity, "%08lx-%04x-4%03x-%04x-%012lx",
           static_cast<unsigned long>(millis()), static_cast<unsigned int>(random(0x10000)),
           static_cast<unsigned int>(random(0x1000)), static_cast<unsigned int>(random(0x10000)),
           static_cast<unsigned long>(random(0x7fffffff)));
}

bool fetchAstronautRoster() {
  lastRosterFetchAttemptMs = millis();
  char response[2048] = {};
  int statusCode = 0;
  if (!apiRequest("GET", "/api/v1/astronauts", nullptr, nullptr, 0, nullptr, nullptr,
                  true, response, sizeof(response), statusCode) || statusCode != 200) {
    Serial.println("Roster fetch failed; check apiAddress, API_SERVER_PORT, and server network access.");
    return false;
  }

  StaticJsonDocument<2048> document;
  DeserializationError jsonError = deserializeJson(document, response);
  if (jsonError) {
    Serial.print("Roster JSON parse failed: ");
    Serial.println(jsonError.c_str());
    Serial.print("Roster response bytes: ");
    Serial.println(strlen(response));
    return false;
  }
  astronautCount = 0;
  JsonArray items = document["items"].as<JsonArray>();
  for (JsonObject item : items) {
    if (astronautCount >= 8) break;
    AstronautOption &option = astronautOptions[astronautCount++];
    copyText(option.id, sizeof(option.id), item["astronaut_id"] | "");
    copyText(option.username, sizeof(option.username), item["username"] | "");
    copyText(option.displayName, sizeof(option.displayName), item["display_name"] | "");
  }
  rosterLoaded = true;
  selectedAstronautIndex = 0;
  workflowState = astronautCount > 0 ? SELECTING_ASTRONAUT : WAITING_FOR_ROSTER;
  drawWorkflowScreen();
  return true;
}

void clearExpiredSession() {
  accessToken[0] = '\0';
  refreshToken[0] = '\0';
  astronautId[0] = '\0';
  astronautUsername[0] = '\0';
  pinDigits[0] = '\0';
  pinLength = 0;
  activeMeasurementId[0] = '\0';
  recordedBpm = 0;
  workflowState = rosterLoaded ? SELECTING_ASTRONAUT : WAITING_FOR_ROSTER;
  drawWorkflowScreen();
  playErrorTone();
}

bool refreshAccessToken() {
  if (refreshToken[0] == '\0') return false;
  char requestBody[256];
  snprintf(requestBody, sizeof(requestBody), "{\"refresh_token\":\"%s\"}", refreshToken);
  char response[1024] = {};
  int statusCode = 0;
  if (!apiRequest("POST", "/api/v1/auth/sessions/refresh", "application/json",
                  reinterpret_cast<const uint8_t *>(requestBody), strlen(requestBody),
                  nullptr, nullptr, false, response, sizeof(response), statusCode) || statusCode != 200) {
    clearExpiredSession();
    Serial.println("Session expired; log in again.");
    return false;
  }
  StaticJsonDocument<512> document;
  if (deserializeJson(document, response)) {
    clearExpiredSession();
    return false;
  }
  const char *newAccessToken = document["access_token"] | "";
  const char *newRefreshToken = document["refresh_token"] | "";
  const unsigned long expiresIn = document["expires_in"] | 0UL;
  if (newAccessToken[0] == '\0' || newRefreshToken[0] == '\0' || expiresIn == 0) {
    clearExpiredSession();
    return false;
  }
  copyText(accessToken, sizeof(accessToken), newAccessToken);
  copyText(refreshToken, sizeof(refreshToken), newRefreshToken);
  accessTokenExpiresAt = millis() + expiresIn * 1000UL;
  return true;
}

bool ensureSession() {
  if (accessToken[0] == '\0') return false;
  if (static_cast<int32_t>(accessTokenExpiresAt - millis()) > TOKEN_REFRESH_MARGIN_MS) return true;
  return refreshAccessToken();
}

bool loginAstronaut(const char *username, const char *pin) {
  if (!rosterLoaded && !fetchAstronautRoster()) return false;
  const AstronautOption *selected = nullptr;
  for (uint8_t index = 0; index < astronautCount; index++) {
    if (strcmp(astronautOptions[index].username, username) == 0) {
      selected = &astronautOptions[index];
      break;
    }
  }
  if (selected == nullptr) {
    Serial.println("Username is not in the device roster.");
    return false;
  }

  char requestBody[192];
  snprintf(requestBody, sizeof(requestBody), "{\"astronaut_id\":\"%s\",\"pin\":\"%s\"}", selected->id, pin);
  char response[1024] = {};
  int statusCode = 0;
  bool loggedIn = apiRequest("POST", "/api/v1/auth/sessions", "application/json",
                             reinterpret_cast<const uint8_t *>(requestBody), strlen(requestBody),
                             nullptr, nullptr, true, response, sizeof(response), statusCode) && statusCode == 201;
  memset(requestBody, 0, sizeof(requestBody));
  if (!loggedIn) {
    Serial.println("Login failed; check username/PIN and API connection.");
    return false;
  }
  StaticJsonDocument<768> document;
  if (deserializeJson(document, response)) {
    Serial.println("Login response was not valid JSON.");
    return false;
  }
  copyText(accessToken, sizeof(accessToken), document["access_token"] | "");
  copyText(refreshToken, sizeof(refreshToken), document["refresh_token"] | "");
  copyText(astronautId, sizeof(astronautId), selected->id);
  copyText(astronautUsername, sizeof(astronautUsername), selected->username);
  accessTokenExpiresAt = millis() + (document["expires_in"] | 900UL) * 1000UL;
  return accessToken[0] != '\0';
}

bool requestCameraCapture(const char *measurementId) {
  WiFiClient client;
  client.setTimeout(CAMERA_READ_TIMEOUT_MS);
  if (!client.connect(cameraAddress, CAMERA_PORT)) {
    Serial.print("[CAMERA] TCP connect failed to ");
    Serial.print(cameraAddress);
    Serial.print(':');
    Serial.println(CAMERA_PORT);
    return false;
  }
  char requestBody[96];
  snprintf(requestBody, sizeof(requestBody), "{\"measurement_id\":\"%s\"}", measurementId);
  client.print("POST /capture HTTP/1.1\r\nHost: ");
  client.print(cameraAddress);
  client.print("\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ");
  client.print(strlen(requestBody));
  client.print("\r\n\r\n");
  client.print(requestBody);
  String statusLine = client.readStringUntil('\n');
  bool accepted = statusLine.indexOf(" 200 ") >= 0 || statusLine.indexOf(" 202 ") >= 0;
  if (!accepted) {
    Serial.print("[CAMERA] Capture request rejected: ");
    Serial.println(statusLine);
  } else {
    Serial.print("[CAMERA] Capture request accepted: ");
    Serial.println(statusLine);
  }
  client.stop();
  return accepted;
}

bool capturePhoto() {
  tone(BUZZER_PIN, 1400, 45);
  if (WiFi.status() != WL_CONNECTED) {
    copyText(workflowNotice, sizeof(workflowNotice), "WIFI NOT CONNECTED");
    drawWorkflowScreen();
    playErrorTone();
    return false;
  }
  if (!ensureSession()) {
    copyText(workflowNotice, sizeof(workflowNotice), "LOGIN EXPIRED");
    drawWorkflowScreen();
    playErrorTone();
    return false;
  }

  workflowState = WAITING_FOR_UPLOAD;
  drawWorkflowScreen();
  if (activeMeasurementId[0] == '\0') {
    char clientMeasurementId[40];
    char idempotencyKey[40];
    makeRequestId(clientMeasurementId, sizeof(clientMeasurementId));
    makeRequestId(idempotencyKey, sizeof(idempotencyKey));
    char requestBody[512];
    snprintf(requestBody, sizeof(requestBody),
             "{\"client_measurement_id\":\"%s\",\"pulse\":{\"bpm\":%d,\"quality\":\"good\",\"algorithm_version\":\"demo-1.0\"},\"photo\":{\"camera_id\":\"%s\",\"media_type\":\"image/jpeg\",\"width\":%u,\"height\":%u}}",
             clientMeasurementId, recordedBpm, CAMERA_ID, CAMERA_FRAME_WIDTH, CAMERA_FRAME_HEIGHT);
    char response[1024] = {};
    int statusCode = 0;
    if (!apiRequest("POST", "/api/v1/measurements", "application/json",
                    reinterpret_cast<const uint8_t *>(requestBody), strlen(requestBody), accessToken,
                    idempotencyKey, true, response, sizeof(response), statusCode) || statusCode != 201) {
      workflowState = TAKING_PHOTO;
      copyText(workflowNotice, sizeof(workflowNotice), "PULSE SEND FAILED");
      drawWorkflowScreen();
      playErrorTone();
      return false;
    }
    StaticJsonDocument<512> document;
    if (deserializeJson(document, response)) {
      workflowState = TAKING_PHOTO;
      copyText(workflowNotice, sizeof(workflowNotice), "BAD API RESPONSE");
      drawWorkflowScreen();
      playErrorTone();
      return false;
    }
    copyText(activeMeasurementId, sizeof(activeMeasurementId), document["measurement_id"] | "");
    if (activeMeasurementId[0] == '\0') {
      workflowState = TAKING_PHOTO;
      copyText(workflowNotice, sizeof(workflowNotice), "NO RECORD ID");
      drawWorkflowScreen();
      playErrorTone();
      return false;
    }
  }

  if (!requestCameraCapture(activeMeasurementId)) {
    workflowState = TAKING_PHOTO;
    copyText(workflowNotice, sizeof(workflowNotice), "CAMERA NOT READY");
    drawWorkflowScreen();
    playErrorTone();
    return false;
  }

  char statusPath[96];
  snprintf(statusPath, sizeof(statusPath), "/api/v1/measurements/%s", activeMeasurementId);
  uint32_t uploadStartedMs = millis();
  bool photoUploaded = false;
  bool photoExpired = false;
  while (millis() - uploadStartedMs < PHOTO_UPLOAD_TIMEOUT_MS) {
    if (!ensureSession()) break;
    char response[1024] = {};
    int statusCode = 0;
    if (apiRequest("GET", statusPath, nullptr, nullptr, 0, accessToken, nullptr,
                   false, response, sizeof(response), statusCode) && statusCode == 200) {
      StaticJsonDocument<512> document;
      if (!deserializeJson(document, response)) {
        const char *photoStatus = document["photo_status"] | "";
        if (strcmp(photoStatus, "uploaded") == 0) {
          photoUploaded = true;
          break;
        }
        if (strcmp(photoStatus, "expired") == 0) {
          photoExpired = true;
          break;
        }
      }
    }
    delay(500);
  }

  if (!photoUploaded) {
    if (photoExpired) activeMeasurementId[0] = '\0';
    workflowState = TAKING_PHOTO;
    copyText(workflowNotice, sizeof(workflowNotice), photoExpired ? "PHOTO EXPIRED - RETRY" : "UPLOAD NOT CONFIRMED");
    drawWorkflowScreen();
    playErrorTone();
    return false;
  }

  for (uint8_t attempt = 0; attempt < 2; attempt++) {
    char response[128] = {};
    int statusCode = 0;
    if (apiRequest("DELETE", "/api/v1/auth/sessions/current", nullptr, nullptr, 0,
                   accessToken, nullptr, false, response, sizeof(response), statusCode) && statusCode == 204) {
      break;
    }
  }
  accessToken[0] = '\0';
  refreshToken[0] = '\0';
  astronautId[0] = '\0';
  astronautUsername[0] = '\0';
  pinDigits[0] = '\0';
  pinLength = 0;
  activeMeasurementId[0] = '\0';
  recordedBpm = 0;
  workflowState = SESSION_COMPLETE;
  completionScreenStartedMs = millis();
  drawWorkflowScreen();
  tone(BUZZER_PIN, 1900, 80);
  return true;
}

void rotateWorkflowSelection(int8_t direction) {
  if (workflowState == SELECTING_ASTRONAUT && astronautCount > 0) {
    int16_t nextIndex = static_cast<int16_t>(selectedAstronautIndex) + direction;
    if (nextIndex < 0) nextIndex = astronautCount - 1;
    if (nextIndex >= astronautCount) nextIndex = 0;
    selectedAstronautIndex = static_cast<uint8_t>(nextIndex);
    playUiTone(1300, 18);
    drawWorkflowScreen();
  } else if (workflowState == ENTERING_PIN) {
    int16_t nextOption = static_cast<int16_t>(selectedPinOption) + direction;
    if (nextOption < 0) nextOption = PIN_OPTION_COUNT - 1;
    if (nextOption >= PIN_OPTION_COUNT) nextOption = 0;
    selectedPinOption = static_cast<uint8_t>(nextOption);
    workflowNotice[0] = '\0';
    playUiTone(1500, 18);
    drawWorkflowScreen();
  }
}

void handleWorkflowClick() {
  if (workflowState == SELECTING_ASTRONAUT && astronautCount > 0) {
    pinDigits[0] = '\0';
    pinLength = 0;
    selectedPinOption = 0;
    workflowNotice[0] = '\0';
    workflowState = ENTERING_PIN;
    playUiTone(1800, 45);
    drawWorkflowScreen();
  } else if (workflowState == ENTERING_PIN) {
    if (selectedPinOption <= 9) {
      if (pinLength < PIN_LENGTH) {
        pinDigits[pinLength++] = static_cast<char>('0' + selectedPinOption);
        pinDigits[pinLength] = '\0';
        playUiTone(1900, 30);
      } else {
        playErrorTone();
      }
    } else if (selectedPinOption == PIN_BACKSPACE_OPTION && pinLength > 0) {
      pinDigits[--pinLength] = '\0';
      playUiTone(1100, 35);
    } else if (selectedPinOption == PIN_LOGIN_OPTION) {
      if (pinLength != PIN_LENGTH) {
        copyText(workflowNotice, sizeof(workflowNotice), "ENTER 6 DIGITS");
        playErrorTone();
      } else if (loginAstronaut(astronautOptions[selectedAstronautIndex].username, pinDigits)) {
        pinDigits[0] = '\0';
        pinLength = 0;
        recordedBpm = 0;
        activeMeasurementId[0] = '\0';
        workflowNotice[0] = '\0';
        workflowState = RECORDING_PULSE;
        playUiTone(2300, 100);
        drawWorkflowScreen();
        return;
      } else {
        copyText(workflowNotice, sizeof(workflowNotice), "LOGIN FAILED");
        playErrorTone();
      }
    } else if (selectedPinOption == PIN_CANCEL_OPTION) {
      pinDigits[0] = '\0';
      pinLength = 0;
      workflowNotice[0] = '\0';
      workflowState = SELECTING_ASTRONAUT;
      playUiTone(900, 45);
      drawWorkflowScreen();
      return;
    }
    drawWorkflowScreen();
  } else if (workflowState == RECORDING_PULSE) {
    bool freshReading = lastBeatMs != 0 && millis() - lastBeatMs <= 3000;
    if (freshReading && heartRateBpm >= MIN_DISPLAY_BPM && heartRateBpm <= MAX_DISPLAY_BPM) {
      recordedBpm = heartRateBpm;
      workflowNotice[0] = '\0';
      workflowState = TAKING_PHOTO;
      playUiTone(2100, 100);
    } else {
      copyText(workflowNotice, sizeof(workflowNotice), "WAIT FOR VALID BPM");
      playErrorTone();
    }
    drawWorkflowScreen();
  } else if (workflowState == TAKING_PHOTO) {
    workflowNotice[0] = '\0';
    capturePhoto();
  }
}

void maintainWorkflow() {
  if (workflowState == SESSION_COMPLETE && millis() - completionScreenStartedMs >= COMPLETE_SCREEN_TIME_MS) {
    selectedAstronautIndex = 0;
    workflowNotice[0] = '\0';
    workflowState = rosterLoaded ? SELECTING_ASTRONAUT : WAITING_FOR_ROSTER;
    drawWorkflowScreen();
  }
}

void serviceEncoder() {
  noInterrupts();
  int16_t newTransitions = encoderTransitions;
  encoderTransitions = 0;
  interrupts();

  encoderTransitionRemainder += newTransitions;
  while (encoderTransitionRemainder >= 4) {
    encoderTransitionRemainder -= 4;
    rotateWorkflowSelection(1);
  }
  while (encoderTransitionRemainder <= -4) {
    encoderTransitionRemainder += 4;
    rotateWorkflowSelection(-1);
  }

  uint32_t now = millis();
  bool buttonReading = digitalRead(ENCODER_SW_PIN);
  if (buttonReading != previousButtonReading) {
    previousButtonReading = buttonReading;
    buttonChangeMs = now;
  }
  if (now - buttonChangeMs >= 30 && buttonReading != stableButtonState) {
    stableButtonState = buttonReading;
    if (stableButtonState == LOW) {
      handleWorkflowClick();
    }
  }
}

bool waitForDhtLevel(uint8_t level, uint32_t timeoutUs, uint32_t &elapsedUs) {
  uint32_t startUs = micros();
  while (digitalRead(DHT_PIN) != level) {
    if (micros() - startUs > timeoutUs) {
      return false;
    }
  }
  elapsedUs = micros() - startUs;
  return true;
}

bool readDht11(int &temperature, int &humidity) {
  uint8_t data[5] = {};
  uint32_t elapsedUs = 0;
  dhtErrorCode = 0;

  pinMode(DHT_PIN, OUTPUT);
  digitalWrite(DHT_PIN, LOW);
  delay(20);
  pinMode(DHT_PIN, INPUT_PULLUP);

  // Released line rises first; the sensor then answers LOW ~80us, HIGH ~80us, then LOW to start bit 1.
  if (!waitForDhtLevel(HIGH, 200, elapsedUs) ||
      !waitForDhtLevel(LOW, 200, elapsedUs) ||
      !waitForDhtLevel(HIGH, 200, elapsedUs) ||
      !waitForDhtLevel(LOW, 200, elapsedUs)) {
    dhtErrorCode = 1;
    return false;
  }

  // Each bit: ~50us LOW, then HIGH for ~27us (0) or ~70us (1).
  for (uint8_t bit = 0; bit < 40; bit++) {
    if (!waitForDhtLevel(HIGH, 200, elapsedUs) ||
        !waitForDhtLevel(LOW, 200, elapsedUs)) {
      dhtErrorCode = 2;
      return false;
    }
    data[bit / 8] <<= 1;
    if (elapsedUs > 45) {
      data[bit / 8] |= 1;
    }
  }

  if (static_cast<uint8_t>(data[0] + data[1] + data[2] + data[3]) != data[4]) {
    dhtErrorCode = 3;
    return false;
  }

  humidity = data[0];
  temperature = data[2];
  if ((data[3] & 0x80) != 0) {
    temperature = -temperature;
  }
  return true;
}

void readTemperature() {
  uint32_t now = millis();
  if (now - lastDhtReadMs < DHT_READ_INTERVAL_MS) {
    return;
  }
  lastDhtReadMs = now;

  temperatureValid = readDht11(temperatureC, humidityPercent);
  if (temperatureValid) {
    Serial.print("DHT11: ");
    Serial.print(temperatureC);
    Serial.print(" C, ");
    Serial.print(humidityPercent);
    Serial.println(" % RH");
  } else {
    Serial.print("DHT11 error: ");
    if (dhtErrorCode == 1) {
      Serial.println("no response; check DATA pin, power, and ground");
    } else if (dhtErrorCode == 2) {
      Serial.println("data timeout; check pull-up and wiring");
    } else {
      Serial.println("checksum error; check wire length and connections");
    }
  }
}

uint32_t medianBeatInterval() {
  uint32_t sortedIntervals[INTERVAL_HISTORY_SIZE];
  for (uint8_t i = 0; i < beatIntervalCount; i++) {
    sortedIntervals[i] = beatIntervals[i];
  }

  for (uint8_t i = 1; i < beatIntervalCount; i++) {
    uint32_t interval = sortedIntervals[i];
    int8_t j = i - 1;
    while (j >= 0 && sortedIntervals[j] > interval) {
      sortedIntervals[j + 1] = sortedIntervals[j];
      j--;
    }
    sortedIntervals[j + 1] = interval;
  }

  return sortedIntervals[beatIntervalCount / 2];
}

void registerBeat(uint32_t beatTime) {
  if (lastBeatMs == 0) {
    lastBeatMs = beatTime;
    drawStatus(true);
    return;
  }

  uint32_t interval = beatTime - lastBeatMs;
  if (interval < MIN_BEAT_INTERVAL_MS) {
    return;
  }
  if (interval > MAX_BEAT_INTERVAL_MS) {
    lastBeatMs = beatTime;
    beatIntervalCount = 0;
    nextIntervalIndex = 0;
    heartRateBpm = 0;
    drawBpm(heartRateBpm);
    drawStatus(true);
    return;
  }

  if (beatIntervalCount >= MIN_INTERVALS_FOR_READING) {
    uint32_t medianInterval = medianBeatInterval();
    if (interval * 100UL < medianInterval * 55UL ||
        interval * 100UL > medianInterval * 180UL) {
      return;
    }
  }

  lastBeatMs = beatTime;
  beatIntervals[nextIntervalIndex] = interval;
  nextIntervalIndex = (nextIntervalIndex + 1) % INTERVAL_HISTORY_SIZE;
  if (beatIntervalCount < INTERVAL_HISTORY_SIZE) {
    beatIntervalCount++;
  }

  if (beatIntervalCount >= MIN_INTERVALS_FOR_READING) {
    heartRateBpm = 60000 / medianBeatInterval();
    drawBpm(heartRateBpm);
    drawReadingStatus();
  } else {
    drawStatus(true);
  }
}

void sampleHeartRate() {
  uint32_t now = millis();
  if (now - lastSampleMs < SAMPLE_INTERVAL_MS) {
    return;
  }
  lastSampleMs = now;

  int rawValue = analogRead(HEART_RATE_PIN);
  if (!filterReady) {
    signalBaseline = rawValue;
    filterReady = true;
  }

  signalBaseline += (rawValue - signalBaseline) * 0.005f;
  float pulseSignal = rawValue - signalBaseline;
  filteredPulse += (pulseSignal - filteredPulse) * 0.25f;
  float magnitude = filteredPulse * SENSOR_POLARITY;
  if (magnitude < 0.0f) {
    magnitude = 0.0f;
  }
  pulseEnvelope += (magnitude - pulseEnvelope) * 0.02f;

  float threshold = pulseEnvelope * 0.55f;
  if (threshold < 8.0f) {
    threshold = 8.0f;
  }

  if (!pulseActive && magnitude >= threshold) {
    pulseActive = true;
    activePulseThreshold = threshold;
    pulseStartMs = now;
  } else if (pulseActive) {
    if (magnitude < activePulseThreshold * 0.65f) {
      pulseActive = false;
      uint32_t pulseWidth = now - pulseStartMs;
      if (pulseWidth >= MIN_PULSE_WIDTH_MS && pulseWidth <= MAX_PULSE_WIDTH_MS) {
        registerBeat(pulseStartMs);
      }
    }
  }

  if (lastBeatMs != 0 && now - lastBeatMs > 3000) {
    lastBeatMs = 0;
    heartRateBpm = 0;
    pulseActive = false;
    beatIntervalCount = 0;
    nextIntervalIndex = 0;
    drawBpm(heartRateBpm);
    drawStatus(false);
  }
}

void connectToWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  lastWifiAttemptMs = millis();
  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.print("Uno WiFi IP: ");
      Serial.println(WiFi.localIP());
      rosterLoaded = false;
    }
    if (!rosterLoaded && millis() - lastRosterFetchAttemptMs >= 5000) fetchAstronautRoster();
    return;
  }
  wifiWasConnected = false;
  if (millis() - lastWifiAttemptMs >= 10000) {
    connectToWifi();
  }
}

void postEnvironmentIfDue() {
  uint32_t now = millis();
  if (!temperatureValid || now - lastEnvironmentPostMs < ENVIRONMENT_POST_INTERVAL_MS || !ensureSession()) {
    return;
  }
  lastEnvironmentPostMs = now;
  char clientReadingId[40];
  char idempotencyKey[40];
  makeRequestId(clientReadingId, sizeof(clientReadingId));
  makeRequestId(idempotencyKey, sizeof(idempotencyKey));
  char requestBody[320];
  snprintf(requestBody, sizeof(requestBody),
           "{\"client_reading_id\":\"%s\",\"client_uptime_ms\":%lu,\"device_id\":\"%s\",\"sensor\":\"DHT11\",\"temperature_c\":%d,\"relative_humidity_percent\":%d}",
           clientReadingId, static_cast<unsigned long>(now), "uno-demo-01", temperatureC, humidityPercent);
  char response[768] = {};
  int statusCode = 0;
  bool sent = apiRequest("POST", "/api/v1/environment-readings", "application/json",
                         reinterpret_cast<const uint8_t *>(requestBody), strlen(requestBody), accessToken,
                         idempotencyKey, true, response, sizeof(response), statusCode) && statusCode == 201;
  Serial.println(sent ? "Environment reading uploaded." : "Environment upload failed.");
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  connectToWifi();
  pinMode(HEART_RATE_PIN, INPUT);
  pinMode(ENCODER_CLK_PIN, INPUT_PULLUP);
  pinMode(ENCODER_DT_PIN, INPUT_PULLUP);
  pinMode(ENCODER_SW_PIN, INPUT_PULLUP);
  previousButtonReading = digitalRead(ENCODER_SW_PIN);
  stableButtonState = previousButtonReading;
  encoderPreviousState = readEncoderState();
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK_PIN), onEncoderChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_DT_PIN), onEncoderChange, CHANGE);
  randomSeed(analogRead(A1) ^ micros());
  gfx->begin();
  drawWorkflowScreen();
}

void loop() {
  serviceEncoder();
  maintainWifi();
  sampleHeartRate();
  readTemperature();
  postEnvironmentIfDue();
  maintainWorkflow();
}
