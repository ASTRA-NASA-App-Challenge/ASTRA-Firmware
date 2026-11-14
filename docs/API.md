# Astronaut Monitoring Demo API

**Status:** Draft contract for a software demo. This is not a validated or certified medical-device interface. Do not use it to make clinical decisions or emergency-response decisions.

## 1. Scope

This API supports a web client and a paired monitoring-device set (Arduino Uno R4 WiFi plus ESP32-CAM) that:

1. Load a roster of astronauts authorized for that device.
2. Let an astronaut choose a username and authenticate with a PIN.
3. Submit pulse metadata from the Uno and have the ESP32-CAM upload the still image directly to the server.
4. Submit ambient temperature and humidity every 30 seconds during an authenticated session.
5. Retrieve measurement history and its associated photo for an authorized viewer.

This repository contains the device firmware and API contract only; a compatible backend must be run separately. The sketches implement the encoder-driven flow: choose an astronaut, enter a PIN, confirm a valid BPM, take a photo, and automatically end the session after the server confirms the photo upload. The ESP32-CAM captures JPEG and uploads directly to the server. These changes have not been compiled or hardware-tested in this environment. The DHT11 measures ambient temperature/humidity, not body temperature. The pulse demo calculates BPM but does not retain a raw waveform.

### Device Roles

- **Uno R4 WiFi:** drives the screen and sensors, handles astronaut selection/login, submits pulse and environmental readings, and asks the camera to capture a photo. It does not receive or display the photo bytes in the proposed direct-upload flow.
- **ESP32-CAM:** captures the still and sends the image bytes directly to the API over HTTPS. It has its own registered device identity and is paired server-side with its Uno.
- **Backend:** binds the pulse record, pending photo, astronaut session, and paired devices; it serves authorized history and photos to the web UI.

The Uno-to-camera trigger is a local device-control message carrying a measurement ID, not an API upload of the image. Keep the image on the camera-to-server path. The local HTTP control interface and camera-to-server HTTP upload are for an isolated demo network only; production use requires authenticated encrypted transport.

## 2. Conventions

- Production base URL example: `https://api.demo.example.com/api/v1`. The local demo server uses `http://<computer-lan-ip>:3000/api/v1`.
- Production API traffic must use HTTPS. The local demo's HTTP is unencrypted and must only be used with sample data on a trusted isolated LAN; do not expose it to the internet.
- JSON fields use `snake_case`; timestamps are ISO 8601 UTC, for example `2026-11-14T10:30:00Z`.
- IDs are opaque strings. Usernames are identifiers, not secrets.
- JSON request and response bodies use `application/json`; photo upload uses a raw binary request body and photo retrieval returns image bytes.
- `Authorization: Bearer <access_token>` is required for astronaut-scoped operations.
- A registered device identity is required for pre-login roster access. Use a unique device certificate or credential; never embed a shared production secret in firmware.
- Use a fresh UUID in `Idempotency-Key` for every measurement and environment event. Reuse the same key when retrying that event.

## 3. Authentication Flow

1. The Uno authenticates as a registered device and loads the authorized astronaut roster. The camera has a separate registered identity paired with that Uno.
2. The astronaut turns the Uno's encoder to choose a roster name and presses to select it.
3. The PIN screen cycles through digits `0` to `9`, `BK` (backspace), `OK` (submit), and `CANCEL`; rotate to an option and press to use it. Exactly six digits are required, and entered digits are masked.
4. The Uno posts the selected `astronaut_id` and PIN to create a session bound to its registered device identity.
5. The Uno displays the live pulse reading. The astronaut presses the encoder to save a fresh, valid BPM; the value is then held for the photo measurement.
6. The astronaut presses again to take the photo. The Uno creates the pending measurement and asks the camera to capture for its ID. The camera uploads directly to the server using its own device identity; no astronaut token is passed to the camera.
7. The Uno waits until the server reports `photo_status: uploaded`, calls `DELETE /auth/sessions/current`, clears local session state, and returns to the roster. If the upload is not confirmed, it remains on the photo step so the astronaut can retry.
8. The Uno uses the access token for 30-second ambient readings during the authenticated flow. Never log the PIN or bearer token.

The roster endpoint returns only the fields needed for selection, and production deployments must scope it to the authenticated device. Avoid exposing a global directory. PINs must be stored as salted, slow password hashes on the server, rate-limited, and never returned by any endpoint. For convenience, the local demo server allows browser clients to call roster/login without a device key; this is only acceptable because its astronaut list and PINs are fake sample data.

## 4. Endpoint Summary

| Method | Path | Purpose | Authorization |
|---|---|---|---|
| `GET` | `/healthz` | Service liveness | None; no sensitive details |
| `GET` | `/astronauts` | List/search astronauts assigned to this device | Device identity |
| `POST` | `/auth/sessions` | Authenticate username selection and PIN | Device identity |
| `POST` | `/auth/sessions/refresh` | Rotate session tokens | Refresh token |
| `GET` | `/auth/sessions/current` | Get current astronaut/session summary | Access token |
| `DELETE` | `/auth/sessions/current` | End current session | Access token |
| `POST` | `/measurements` | Create pending measurement with pulse metadata and photo expectations | Astronaut access token + bound Uno identity |
| `GET` | `/measurements/{measurement_id}` | Get measurement/photo upload status | Access token with record access |
| `PUT` | `/measurements/{measurement_id}/photo` | Upload photo bytes directly from paired ESP32-CAM | Paired camera identity |
| `GET` | `/astronauts/{astronaut_id}/measurements` | Read authorized measurement history | Viewer access token |
| `GET` | `/measurements/{measurement_id}/photo` | Retrieve a measurement photo | Viewer access token |
| `POST` | `/environment-readings` | Submit one ambient reading (normally every 30 seconds) | Access token |

## 5. Device Health

### `GET /healthz`

Returns service availability only. It must not expose configuration, credentials, roster information, or internal stack traces.

**200 response**

```json
{
  "status": "ok",
  "api_version": "v1",
  "time": "2026-11-14T10:30:00Z"
}
```

## 6. Astronaut Roster

### `GET /astronauts?active=true&limit=50&cursor=<opaque>`

Requires registered device authentication. Returns the active astronauts assigned to this device or crew. Supports optional `query=<prefix-or-substring>` for larger rosters and cursor pagination. The UI can show `username` and `display_name`; it must not show PINs or medical data in this response.

**200 response**

```json
{
  "items": [
    {
      "astronaut_id": "ast_01J9W8Y5M4",
      "username": "a.rivera",
      "display_name": "Alex Rivera"
    },
    {
      "astronaut_id": "ast_01J9W91T2K",
      "username": "m.chen",
      "display_name": "Morgan Chen"
    }
  ],
  "next_cursor": null
}
```

`username` is not an authentication factor. The server must scope the roster to the device and rate-limit requests to reduce account enumeration.

## 7. Login and Session

### `POST /auth/sessions`

Requires registered device authentication. `pin` is a string of exactly six numeric digits to preserve leading zeroes.

**Request**

```json
{
  "astronaut_id": "ast_01J9W8Y5M4",
  "pin": "048271"
}
```

**201 response**

```json
{
  "access_token": "<opaque-or-signed-token>",
  "token_type": "Bearer",
  "expires_in": 900,
  "refresh_token": "<rotating-refresh-token>",
  "refresh_expires_in": 28800,
  "session": {
    "session_id": "ses_01J9WA2R8N",
    "astronaut": {
      "astronaut_id": "ast_01J9W8Y5M4",
      "username": "a.rivera",
      "display_name": "Alex Rivera"
    }
  }
}
```

Access tokens expire after 15 minutes. Refresh tokens expire after 8 hours, rotate on every use, and are invalidated at logout. Limit failed PIN attempts (for example, 5 attempts per account/device per 15 minutes); return the same generic authentication error for unknown users and incorrect PINs.

**Errors:** `400` malformed request, `401` invalid credentials, `423` temporary lockout, `429` rate limit.

### `POST /auth/sessions/refresh`

**Request**

```json
{ "refresh_token": "<current-refresh-token>" }
```

**200 response** uses the same token fields as login and returns a newly rotated refresh token. The old refresh token must stop working.

### `GET /auth/sessions/current`

**200 response**

```json
{
  "session_id": "ses_01J9WA2R8N",
  "astronaut": {
    "astronaut_id": "ast_01J9W8Y5M4",
    "username": "a.rivera",
    "display_name": "Alex Rivera"
  },
  "expires_at": "2026-11-14T10:45:00Z"
}
```

### `DELETE /auth/sessions/current`

Invalidates the access/refresh session. Returns `204 No Content`.

## 8. Pulse Measurement and Photo

### `POST /measurements`

Requires the astronaut access token for a session bound to the registered Uno and an `Idempotency-Key` UUID. This endpoint accepts JSON pulse metadata and creates a pending record; it does **not** accept image bytes. The authenticated Uno identity and its paired camera are determined by server-side device registration, not trusted from a body-supplied device ID.

**Request**

```json
{
  "client_measurement_id": "01932c76-4200-7c40-a4a0-3f65039c2cd9",
  "measured_at": "2026-11-14T10:30:00Z",
  "pulse": {
    "bpm": 72,
    "quality": "good",
    "algorithm_version": "demo-1.0",
    "waveform": {
      "sample_rate_hz": 200,
      "adc_resolution_bits": 10,
      "samples": [510, 514, 522, 535, 548, 541, 526, 515]
    }
  },
  "photo": {
    "camera_id": "cam-crew-03",
    "media_type": "image/jpeg",
    "width": 160,
    "height": 120
  }
}
```

The requested `camera_id` must match the camera paired with the authenticated Uno; the server must reject mismatches. `measured_at` is optional for boards without a real-time clock; when omitted, the server assigns its receive time. The waveform is optional because the current firmware only calculates BPM. If submitted, cap waveform duration and size (recommended maximum 20 seconds / 4,000 samples at 200 Hz) and validate each sample against the declared ADC resolution. `quality` is one of `good`, `fair`, `poor`, or `unknown`; it describes signal acquisition quality, not health status. `bpm` must be an integer from 0 to 250; use `0` only when no valid BPM was computed and include a quality/status reason in an implementation extension.

Supported demo dimensions are `160x120` and `320x240`. The server returns the pending measurement ID; the Uno sends only that ID to the paired camera's local capture-control interface. The camera captures a still without returning pixels to the Uno, then uploads directly to the server. No astronaut access or refresh token is forwarded to the camera.

**201 response**

```json
{
  "measurement_id": "mea_01J9WB8KQG",
  "client_measurement_id": "01932c76-4200-7c40-a4a0-3f65039c2cd9",
  "astronaut_id": "ast_01J9W8Y5M4",
  "created_at": "2026-11-14T10:30:02Z",
  "photo_status": "awaiting_upload",
  "photo_upload_expires_at": "2026-11-14T10:35:02Z"
}
```

The pending upload expires after five minutes. Repeating the same request with the same idempotency key returns the same measurement. **Errors:** `400` malformed metadata, `401` missing/expired session, `403` device/camera pairing mismatch, `409` idempotency key reused for different content, `422` invalid dimensions/range, `429` rate limit.

### `PUT /measurements/{measurement_id}/photo`

Called by the paired ESP32-CAM after capture. Requires the camera's unique registered device identity (prefer mutual TLS); it does not accept the astronaut bearer token. The server verifies that the measurement is awaiting a photo and was created by the Uno paired to this camera. The body is the raw image bytes, not multipart data. `Content-Type` must match the photo metadata registered when the pending measurement was created.

For `image/jpeg`, send JPEG bytes. For the current camera's RGB565 output, use `Content-Type: application/vnd.demo.rgb565`; the raw body must be exactly `width * height * 2` bytes and use the byte order agreed during device provisioning (recommended demo value: `big_endian`). Supported demo dimensions are `160x120` and `320x240`. The camera should stream the frame to HTTPS rather than buffer additional copies in RAM. The server should normalize/convert the image for web display and preserve the original only if retention policy allows it.

**Request headers**

```http
Content-Type: image/jpeg
Idempotency-Key: 01932c78-4015-7c40-a1ad-73cf601cd9e5
```

**200 response**

```json
{
  "measurement_id": "mea_01J9WB8KQG",
  "photo_status": "uploaded",
  "received_at": "2026-11-14T10:30:05Z"
}
```

The camera retries with the same idempotency key if the response is lost. Re-uploading identical bytes is idempotent; attempting to replace an already accepted photo with different bytes returns `409`.

**Errors:** `401` unknown camera identity, `403` camera is not paired with the measurement's Uno, `404` unknown measurement, `409` conflicting duplicate upload, `410` pending upload expired/already closed, `413` image too large, `415` unsupported media type, `422` incorrect image dimensions/length or invalid image, `429` rate limit.

### Local Uno-to-camera capture control (not a backend endpoint)

The Uno needs to tell the camera which pending measurement to photograph. A proposed local control interface is:

`POST http://<camera-host>/capture`

```json
{ "measurement_id": "mea_01J9WB8KQG" }
```

The camera returns `202 Accepted` with `{ "measurement_id": "mea_01J9WB8KQG", "status": "capture_queued" }`, captures a still, and uploads it directly to `PUT /api/v1/measurements/{measurement_id}/photo`. The control response contains no image bytes. The Uno can poll `GET http://<camera-host>/capture/{measurement_id}/status` for `queued`, `capturing`, `uploaded`, or `failed`, or poll the backend measurement resource. The existing camera `GET /capture` image-return behavior should be replaced or disabled in this flow.

The local control interface is implementation-specific and is not the public web API. Restrict it to the paired Uno; use authenticated transport where available, and keep it on an isolated demo network otherwise. A measurement ID is not a credential, but the camera must still reject IDs that are not pending for its paired Uno.

### `GET /measurements/{measurement_id}`

Requires an access token authorized for the record. The Uno or web client can poll this resource to learn whether the camera upload completed.

**200 response while waiting**

```json
{
  "measurement_id": "mea_01J9WB8KQG",
  "measured_at": "2026-11-14T10:30:00Z",
  "pulse": { "bpm": 72, "quality": "good" },
  "photo_status": "awaiting_upload",
  "photo_url": null
}
```

When upload completes, `photo_status` is `uploaded` and `photo_url` is `/api/v1/measurements/{measurement_id}/photo`. The photo URL remains access-controlled; it is not a public URL.

## 9. Environment Telemetry

### `POST /environment-readings`

Requires an astronaut access token and an `Idempotency-Key` UUID. The client should send one reading every 30 seconds while a session is active. The server associates it with the authenticated astronaut/session and verifies the informational `device_id` against the Uno bound to that session; do not trust astronaut or device identity supplied only in the body.

**Request**

```json
{
  "client_reading_id": "01932c77-9de1-7f6a-9c2c-cfe3b4056c70",
  "measured_at": "2026-11-14T10:30:30Z",
  "device_id": "uno-r4-crew-03",
  "sensor": "DHT11",
  "temperature_c": 22,
  "relative_humidity_percent": 46
}
```

`measured_at` is optional when a device has no clock; the demo server uses receive time in that case. `temperature_c` is ambient temperature in Celsius, not body temperature. Humidity is relative humidity from 0 to 100 percent. Values must be finite and within sensor/physical bounds; reject invalid readings rather than silently clamping them.

**201 response**

```json
{
  "reading_id": "env_01J9WC12S8",
  "client_reading_id": "01932c77-9de1-7f6a-9c2c-cfe3b4056c70",
  "astronaut_id": "ast_01J9W8Y5M4",
  "received_at": "2026-11-14T10:30:31Z",
  "status": "accepted"
}
```

**Errors:** `400` malformed body, `401` missing/expired session, `409` duplicate key with different content, `422` out-of-range values, `429` rate limit.

## 10. History and Photo Retrieval

### `GET /astronauts/{astronaut_id}/measurements?from=<UTC>&to=<UTC>&limit=50&cursor=<opaque>`

Requires a viewer token whose role/scope allows access to this astronaut. Ordinary astronaut tokens can only read their own records. Maximum query window and page size should be bounded.

**200 response**

```json
{
  "items": [
    {
      "measurement_id": "mea_01J9WB8KQG",
      "measured_at": "2026-11-14T10:30:00Z",
      "pulse": { "bpm": 72, "quality": "good" },
      "photo_url": "/api/v1/measurements/mea_01J9WB8KQG/photo"
    }
  ],
  "next_cursor": null
}
```

### `GET /measurements/{measurement_id}/photo`

Requires authorization to view the associated record. Returns normalized `image/jpeg` bytes with `Cache-Control: private, no-store`; never use a public, permanent photo URL.

## 11. Common Error Shape

All non-2xx JSON responses use this envelope and a unique request ID:

```json
{
  "error": {
    "code": "INVALID_REQUEST",
    "message": "The request could not be accepted.",
    "request_id": "req_01J9WD0P2V",
    "details": []
  }
}
```

Do not return stack traces, PIN validity details, tokens, or sensitive astronaut data in errors. Use `application/json` for JSON errors and the correct status code.

## 12. Data Types

```ts
export type Astronaut = {
  astronaut_id: string;
  username: string;
  display_name: string;
};

export type PulseMeasurement = {
  bpm: number;
  quality: "good" | "fair" | "poor" | "unknown";
  algorithm_version: string;
  waveform?: {
    sample_rate_hz: number;
    adc_resolution_bits: number;
    samples: number[];
  };
};

export type EnvironmentReading = {
  client_reading_id: string;
  measured_at: string;
  device_id: string;
  sensor: "DHT11";
  temperature_c: number;
  relative_humidity_percent: number;
};
```

## 13. Security, Privacy, and Reliability Requirements

- Use TLS with certificate validation for every production backend connection, including the ESP32-CAM photo upload. The current firmware uses plain HTTP for local demo connections and must stay on an isolated trusted LAN with sample data only.
- Require unique identities for the Uno and camera, rotate/revoke compromised devices, authorize the Uno only for its assigned roster, and authorize the camera only for measurements created by its paired Uno.
- Hash PINs with a password-hashing algorithm such as Argon2id or bcrypt; use rate limits, lockouts, audit logs, and generic login failures.
- Never put PINs, bearer tokens, or health data in URLs, analytics, crash reports, or ordinary logs. Do not hardcode reusable production credentials in either sketch.
- Encrypt data at rest, enforce least-privilege access, define deletion/retention periods, and audit history/photo access.
- Require idempotency for retries. Queue events only in protected storage, preserve original measurement timestamps, and mark delayed uploads as delayed.
- Validate input sizes before allocation. Recommended demo upload limit: 5 MB per photo and 20 seconds per waveform. Stream camera frames to the server so the ESP32-CAM does not need to hold multiple image copies in memory.
- Show a clear sensor/transfer failure state. Never substitute a previous BPM/temperature reading while presenting it as current.
- Obtain appropriate privacy, security, clinical, and regulatory review before using this design with real astronaut health data or as a medical device.

## 14. Device Firmware Setup

- This repository does not include a backend server. Run a compatible implementation of the endpoints in this contract. The firmware expects the API on port `5000` and uses plain HTTP for local demo connections only.
- Copy each board's `device_config.example.h` to `device_config.h`. Configure Wi-Fi credentials, the computer's LAN IP, device IDs/keys, and the camera IP in the Uno's local file. Keep these local configuration files out of version control.
- Keep both boards and the backend computer on the same LAN. Set `apiAddress` in the Uno configuration and `API_HOST` in the camera configuration to the computer's LAN IPv4 address; set the Uno's `cameraAddress` to the ESP32-CAM's IP.
- Install ArduinoJson and Arduino_GFX_Library using Arduino IDE Library Manager. WiFiS3 and esp_camera are provided by their respective board packages. The Serial Monitor runs at 115200 baud; astronaut selection, six-digit PIN entry, BPM confirmation, and photo capture use the rotary encoder.
- Turn the encoder to choose a name and press. Enter exactly six PIN digits by rotating to each digit and pressing to append it; use `BK` to correct a digit, then choose `OK` and press to log in. After the live BPM is valid, press to record it, then press again to take the photo. The server-confirmed upload automatically logs the astronaut out.
- Connect a piezo buzzer between D12 and GND for encoder, error, and completion tones. Do not connect a low-impedance loudspeaker directly to a GPIO pin; use a transistor driver.
- The Uno posts valid DHT11 temperature/humidity readings every 30 seconds during an authenticated session. Backend storage, photo retention, and session persistence depend on the separately deployed server.

## 15. Production Work Still Required

- Replace demo HTTP with TLS and certificate validation on both boards; provision unique device identities and rotate credentials.
- Replace in-memory data, sample PINs, and default device keys with a persistent backend, production-grade PIN hashing/authentication, authorization, audit, retention, and deletion policies.
- Add a real-time clock or trusted time synchronization if client measurement timestamps are required.
- Capture/buffer a pulse waveform if a true pulse scan is required; the present code only derives BPM.
- Build the production web UI and validate sensor behavior, image handling, network recovery, and device pairing end to end.
- Treat DHT11 temperature/humidity and the optical pulse sensor as demo-grade measurements; validate sensors and algorithms before any safety-critical use.
