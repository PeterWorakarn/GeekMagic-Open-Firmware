# Implementation Plan: Locket Image Display

> Fetch รูป JPEG จาก URL ที่ user กำหนด ทุก N นาที แล้ว decode + วาดเต็มจอ 240x240 (RGB565) วนไปเรื่อย ๆ บน ESP8266.

---

## 1. Overview

Flow ทั้งหมด:

```
User เปิด /locket.html → กรอก URL + Interval (นาที) → กด Save
        │
        ▼
POST /api/v1/locket/config  (Bearer token)
        │  validate + ConfigManager.save() → config.json
        ▼
loop() millis-timer: ถึงเวลา fetch (ทุก N นาที หรือ trigger ครั้งแรก)
        │
        ▼
ImageFetcher::fetchAndDisplay()
        ├─ HTTP GET URL  (HTTPClient + WiFiClient/WiFiClientSecure)
        ├─ stream เขียนลง LittleFS  /cache/locket.jpg   (ไม่โหลดทั้งก้อนเข้า RAM)
        ├─ JPEGDEC.open(file)  → decode ทีละ MCU block
        │     └─ callback วาด pixel ลงจอผ่าน DisplayManager::getGfx()->draw16bitRGBBitmap()
        └─ close + log free heap
```

จุดสำคัญสำหรับ ESP8266:
- **ห้ามโหลดรูปทั้งก้อนเข้า RAM** (heap มีแค่ ~40-80KB) → ต้อง stream ลง LittleFS ก่อน แล้ว decode จากไฟล์
- **ห้าม block loop() นานเกิน ~2s** (watchdog `WDTO_2S`) → ระหว่าง fetch/decode ต้อง `yield()` / `wdtFeed()` เป็นระยะ
- JPEGDEC decode ทีละ MCU แล้ว push เข้าจอทันที → peak RAM ต่ำมาก

---

## 2. Architecture Decision

### 2.1 JPEG Decoder library: `bitbank2/JPEGDEC`

เลือก **JPEGDEC** เพราะ:
- เขียนโดย Larry Bank (คนเดียวกับ AnimatedGIF ที่โปรเจกต์ใช้อยู่แล้ว) → coding style/ API คุ้นเคย, integrate กับ Arduino_GFX ได้ตรง ๆ
- **Block-based decode**: decode ทีละ MCU (เช่น 16x16) แล้วเรียก `JPEG_DRAW_CALLBACK` ส่ง buffer เล็ก ๆ (RGB565 อยู่แล้ว) มาวาด → ไม่ต้องมี framebuffer 240x240x2 = 115KB ใน RAM (ESP8266 ไม่มีทางพอ)
- รองรับ `openFLASH` / `open(File)` อ่าน input จาก LittleFS แบบ streaming ผ่าน callback (`JPEG_READ_CALLBACK`) → input ก็ไม่กิน RAM
- มี option `setPixelType(RGB565_BIG_ENDIAN)` ตรงกับ ST7789 / Arduino_GFX `draw16bitRGBBitmap()`
- รองรับ hardware scaling ในตัว (`JPEG_SCALE_HALF`, `_QUARTER`, `_EIGHTH`) เผื่อรูปใหญ่กว่า 240px

ทางเลือกอื่นที่**ไม่เลือก**:
- `TJpg_Decoder` — ใช้ได้แต่ผูกกับ TFT_eSPI เป็นหลัก, integration กับ Arduino_GFX ต้องเขียน glue เพิ่ม
- decode บน server — ขัดกับ requirement (user กรอก URL ภายนอกตรง ๆ)

### 2.2 รูปแบบ image ที่ support

- **JPEG เท่านั้น** (baseline + progressive ที่ JPEGDEC รองรับ). ขนาดแนะนำ ≤ 240x240; ถ้าใหญ่กว่าใช้ scale flag หรือ crop ตอนวาด
- ขนาดไฟล์แนะนำ ≤ ~200KB (LittleFS partition ~2MB แต่ flash write ช้า + ใช้ร่วมกับ GIF)
- **ไม่รองรับ** PNG/WebP/GIF static ใน feature นี้ (GIF มี path แยกอยู่แล้ว)

### 2.3 Cache บน LittleFS

- เขียนลง `/cache/locket.jpg` (สร้าง dir `/cache` ถ้ายังไม่มี — pattern เดียวกับ `/gif` ใน `handleGifUploadStart`)
- เขียนเป็น `.tmp` ก่อน แล้ว rename ทับ เพื่อกัน partial file ตอน fetch fail (ถ้า rename ไม่ support ก็ลบ tmp เมื่อ fail)
- decode จากไฟล์ cache → ถ้า fetch รอบใหม่ fail ยังมีรูปเดิมค้างจอได้ (ไม่ต้องวาดใหม่)

### 2.4 HTTP fetch

- ใช้ `ESP8266HTTPClient` + `WiFiClient` (http) หรือ `WiFiClientSecure` + `setInsecure()` (https)
- ตรวจ scheme จาก URL prefix; https บน ESP8266 กิน RAM เพิ่มจาก TLS buffer — แนะนำ `client.setBufferSizes(512, 512)` เพื่อลด RAM
- stream `http.getStreamPtr()` → เขียนเป็น chunk เล็ก (เช่น 512B) ลงไฟล์ พร้อม `wdtFeed()` ทุก chunk

---

## 3. ไฟล์ที่ต้องสร้างใหม่

### 3.1 `include/imagefetch/ImageFetcher.h`

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef IMAGE_FETCHER_H
#define IMAGE_FETCHER_H

#include <Arduino.h>

/**
 * @brief Fetch a JPEG image from a URL, cache to LittleFS and draw it full screen.
 *
 * Static utility class. Driven by a millis()-based timer in loop().
 * Designed for ESP8266: streams download to flash, decodes block-by-block.
 */
class ImageFetcher {
   public:
    /// Cache path on LittleFS where the downloaded JPEG is stored.
    static constexpr const char* CACHE_PATH = "/cache/locket.jpg";
    static constexpr const char* CACHE_TMP_PATH = "/cache/locket.tmp";

    /**
     * @brief Init internal timer state. Call once in setup() after config load.
     */
    static void begin();

    /**
     * @brief Call every loop(). Triggers a fetch when the configured interval elapses.
     *        Non-blocking between intervals; the actual fetch+decode blocks briefly
     *        (with wdtFeed/yield) only when it runs.
     */
    static void loop();

    /**
     * @brief Force a fetch + display right now (used right after Save).
     * @return true on success (image fetched and drawn).
     */
    static bool fetchAndDisplayNow();

    /// Last status string (for /api/v1/locket/config GET response / debugging).
    static const String& lastStatus();
    static bool lastOk();

   private:
    static bool downloadToCache(const String& url);  ///< HTTP GET -> /cache/locket.tmp -> rename
    static bool decodeAndDraw();                      ///< JPEGDEC decode CACHE_PATH -> screen

    static unsigned long lastFetchMs_;
    static bool firstRunPending_;
    static String lastStatus_;
    static bool lastOk_;
};

#endif  // IMAGE_FETCHER_H
```

### 3.2 `src/imagefetch/ImageFetcher.cpp`

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "imagefetch/ImageFetcher.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <JPEGDEC.h>
#include <Logger.h>

#include "config/ConfigManager.h"
#include "display/DisplayManager.h"

extern ConfigManager configManager;

// ---- static state ----
unsigned long ImageFetcher::lastFetchMs_ = 0;
bool ImageFetcher::firstRunPending_ = false;
String ImageFetcher::lastStatus_ = "idle";
bool ImageFetcher::lastOk_ = false;

namespace {
constexpr size_t DL_CHUNK = 512;            // download write chunk size (bytes)
constexpr unsigned long MIN_INTERVAL_MS = 60UL * 1000UL;  // safety floor: 1 min
constexpr int HTTP_TIMEOUT_MS = 15000;
constexpr int16_t SCREEN_W = 240;
constexpr int16_t SCREEN_H = 240;

JPEGDEC jpeg;  // decoder instance (small, fine as global)

/**
 * @brief JPEGDEC draw callback. Pushes one decoded MCU block to the display.
 *        pDraw->pPixels is RGB565 (big-endian, matching draw16bitRGBBitmap).
 */
int jpegDrawCallback(JPEGDRAW* pDraw) {
    Arduino_GFX* gfx = DisplayManager::getGfx();
    if (gfx == nullptr) {
        return 0;  // abort decode
    }
    // Clip blocks that fall outside the 240x240 panel (if image > screen).
    if (pDraw->x >= SCREEN_W || pDraw->y >= SCREEN_H) {
        return 1;
    }
    gfx->draw16bitRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    EspClass::wdtFeed();  // each block is small, but keep the dog happy
    return 1;             // continue decoding
}
}  // namespace

void ImageFetcher::begin() {
    lastFetchMs_ = 0;
    firstRunPending_ = (strlen(configManager.getLocketUrl()) > 0);
    lastStatus_ = "ready";
    if (!LittleFS.exists("/cache")) {
        LittleFS.mkdir("/cache");
    }
}

void ImageFetcher::loop() {
    const char* url = configManager.getLocketUrl();
    if (url == nullptr || strlen(url) == 0) {
        return;  // feature disabled (no URL configured)
    }

    unsigned long intervalMs = (unsigned long)configManager.getLocketIntervalMin() * 60UL * 1000UL;
    if (intervalMs < MIN_INTERVAL_MS) {
        intervalMs = MIN_INTERVAL_MS;
    }

    unsigned long now = millis();
    bool due = firstRunPending_ || (now - lastFetchMs_ >= intervalMs);
    if (!due) {
        return;
    }

    firstRunPending_ = false;
    lastFetchMs_ = now;
    fetchAndDisplayNow();
}

bool ImageFetcher::fetchAndDisplayNow() {
    const char* url = configManager.getLocketUrl();
    if (url == nullptr || strlen(url) == 0) {
        lastStatus_ = "no url configured";
        lastOk_ = false;
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        lastStatus_ = "wifi not connected";
        lastOk_ = false;
        Logger::warn("Locket fetch skipped: WiFi down", "Locket");
        return false;
    }

    Logger::info((String("Locket fetching: ") + url).c_str(), "Locket");

    if (!downloadToCache(String(url))) {
        lastOk_ = false;
        return false;  // lastStatus_ set inside
    }

    if (!decodeAndDraw()) {
        lastStatus_ = "decode failed";
        lastOk_ = false;
        Logger::error("Locket JPEG decode failed", "Locket");
        return false;
    }

    lastStatus_ = "ok";
    lastOk_ = true;
    Logger::info((String("Locket displayed, free heap: ") + String(ESP.getFreeHeap())).c_str(), "Locket");
    return true;
}

bool ImageFetcher::downloadToCache(const String& url) {
    std::unique_ptr<WiFiClient> client;
    if (url.startsWith("https://")) {
        auto* secure = new WiFiClientSecure();
        secure->setInsecure();                 // no cert validation (ESP8266 RAM constraint)
        secure->setBufferSizes(512, 512);      // shrink TLS buffers
        client.reset(secure);
    } else {
        client.reset(new WiFiClient());
    }

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(*client, url)) {
        lastStatus_ = "http begin failed";
        return false;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        lastStatus_ = String("http ") + String(code);
        http.end();
        return false;
    }

    if (!LittleFS.exists("/cache")) {
        LittleFS.mkdir("/cache");
    }
    File out = LittleFS.open(CACHE_TMP_PATH, "w");
    if (!out) {
        lastStatus_ = "cache open failed";
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[DL_CHUNK];
    int contentLen = http.getSize();       // -1 if chunked/unknown
    size_t written = 0;

    while (http.connected() && (contentLen > 0 || contentLen == -1)) {
        size_t avail = stream->available();
        if (avail > 0) {
            int toRead = (avail > sizeof(buf)) ? sizeof(buf) : (int)avail;
            int n = stream->readBytes(buf, toRead);
            if (n <= 0) {
                break;
            }
            out.write(buf, n);
            written += n;
            if (contentLen > 0) {
                contentLen -= n;
            }
        } else {
            if (contentLen == -1 && !stream->connected()) {
                break;  // chunked stream finished
            }
            delay(1);  // yield to WiFi/system stack
        }
        EspClass::wdtFeed();
        yield();
    }

    out.close();
    http.end();

    if (written == 0) {
        LittleFS.remove(CACHE_TMP_PATH);
        lastStatus_ = "empty download";
        return false;
    }

    // atomic-ish swap
    LittleFS.remove(CACHE_PATH);
    if (!LittleFS.rename(CACHE_TMP_PATH, CACHE_PATH)) {
        LittleFS.remove(CACHE_TMP_PATH);
        lastStatus_ = "cache rename failed";
        return false;
    }

    Logger::info((String("Locket downloaded ") + String(written) + " bytes").c_str(), "Locket");
    return true;
}

bool ImageFetcher::decodeAndDraw() {
    File f = LittleFS.open(CACHE_PATH, "r");
    if (!f) {
        lastStatus_ = "cache read failed";
        return false;
    }

    // JPEGDEC reads via File* user pointer; the Arduino integration provides
    // open(File &, JPEG_DRAW_CALLBACK). pixel type RGB565 big-endian matches GFX.
    if (!jpeg.open(f, jpegDrawCallback)) {
        f.close();
        lastStatus_ = "jpeg open failed";
        return false;
    }
    jpeg.setPixelType(RGB565_BIG_ENDIAN);

    DisplayManager::clearScreen();  // wipe previous image first

    // decode() returns 1 on success, 0 on failure. Blocks may exceed screen -> clipped in cb.
    int rc = jpeg.decode(0, 0, 0);  // x=0, y=0, no scale flag
    jpeg.close();
    f.close();

    if (rc != 1) {
        lastStatus_ = "jpeg decode rc!=1";
        return false;
    }
    return true;
}

const String& ImageFetcher::lastStatus() { return lastStatus_; }
bool ImageFetcher::lastOk() { return lastOk_; }
```

> หมายเหตุ: API ของ JPEGDEC สำหรับอ่านจาก `File` ขึ้นกับเวอร์ชัน — บางเวอร์ชันใช้ `jpeg.open(File&, callback)` ตรง ๆ, บางเวอร์ชันต้องใช้ `openFLASH`/custom read callback. ตอน implement ให้เช็ค `examples/` ของ JPEGDEC ที่ PlatformIO ดึงมา แล้วปรับ `decodeAndDraw()` ให้ตรง signature จริง. ถ้าเวอร์ชันไม่มี `open(File&, ...)` ให้ใช้ custom callbacks: `open(filename, openCB, closeCB, readCB, seekCB, drawCB)` โดย wrap `LittleFS.open`.

---

## 4. ไฟล์ที่ต้องแก้ไข

### 4.1 `include/config/ConfigManager.h`

เพิ่ม fields + accessors (วางในกลุ่ม public ใกล้ `ntp_server`):

```cpp
    std::string locket_url;
    int locket_interval_min = 5;

    const char* getLocketUrl() const { return locket_url.c_str(); }
    void setLocketUrl(const char* s) {
        if (s) locket_url = s;
    }
    int getLocketIntervalMin() const { return locket_interval_min; }
    void setLocketIntervalMin(int m) {
        if (m > 0) locket_interval_min = m;
    }
```

### 4.2 `src/config/ConfigManager.cpp`

**ใน `load()`** — หลังบรรทัด `this->lcd_rotation = doc["lcd_rotation"] | lcd_rotation;`:

```cpp
    this->locket_url = (doc["locket_url"] | "");
    this->locket_interval_min = doc["locket_interval_min"] | 5;
```

**ใน `save()`** — หลังบล็อก `if (!this->ntp_server.empty()) {...}`:

```cpp
    if (!this->locket_url.empty()) {
        doc["locket_url"] = this->locket_url.c_str();
    }
    doc["locket_interval_min"] = this->locket_interval_min;
```

> ⚠️ ระวัง: `save()` สร้าง `JsonDocument doc;` ใหม่ทุกครั้ง และเขียนทับ `config.json` ทั้งไฟล์ → ต้องเพิ่ม locket fields ใน `save()` ไม่งั้นจะหายตอน save ครั้งถัดไป (เช่นเปลี่ยน rotation). field อื่น ๆ (ssid/password/api_token) เก็บใน SecureStorage จึงไม่อยู่ใน json อยู่แล้ว.

### 4.3 `src/main.cpp`

**เพิ่ม include** (กลุ่มบนสุดกับ include อื่น):
```cpp
#include "imagefetch/ImageFetcher.h"
```

**ใน `setup()`** — หลัง `registerApiEndpoints(webserver);` (config ถูก load แล้ว, WiFi/display พร้อมแล้ว):
```cpp
    ImageFetcher::begin();
```

**ใน `loop()`** — เพิ่มหลัง `DisplayManager::update();`:
```cpp
    ImageFetcher::loop();
```

> `ImageFetcher::loop()` จะ return ทันทีถ้ายังไม่ถึงเวลา/ไม่มี URL → ไม่กระทบ watchdog ในรอบปกติ. รอบที่ fetch จริงจะ block ชั่วคราวแต่มี `wdtFeed()`/`yield()` ภายใน.

### 4.4 `src/web/Api.cpp`

**ใน `registerApiEndpoints()`** — เพิ่ม 2 routes (ก่อน `onNotFound`):
```cpp
    // @openapi {get} /locket/config version=v1 group=Locket summary="Get locket image config" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/locket/config", HTTP_GET, [webserver]() { handleLocketConfigGet(webserver); });

    // @openapi {post} /locket/config version=v1 group=Locket summary="Set locket image config" requiresAuth=true
    // requestBody=application/json requestBodySchema=url:string,interval_min:integer
    // example={"url":"https://example.com/photo.jpg","interval_min":5}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/locket/config", HTTP_POST, [webserver]() { handleLocketConfigSet(webserver); });
```

**เพิ่ม include** ที่หัวไฟล์ (เพื่อเรียก `ImageFetcher::fetchAndDisplayNow()`):
```cpp
#include "imagefetch/ImageFetcher.h"
```

**เพิ่ม handler functions** (ท้ายไฟล์, ตาม style ของ `handleNtpConfigGet/Set`):
```cpp
/**
 * @brief Get Locket image configuration
 */
void handleLocketConfigGet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    JsonDocument doc;
    doc["url"] = configManager.getLocketUrl();
    doc["interval_min"] = configManager.getLocketIntervalMin();
    doc["lastStatus"] = ImageFetcher::lastStatus();
    doc["lastOk"] = ImageFetcher::lastOk();

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
}

/**
 * @brief Set Locket image configuration and trigger an immediate fetch
 */
void handleLocketConfigSet(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }

    if (!webserver->raw().hasArg("plain") || webserver->raw().arg("plain").length() == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Missing JSON body";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }

    String body = webserver->raw().arg("plain");
    JsonDocument ddoc;
    DeserializationError err = deserializeJson(ddoc, body);
    if (err) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Invalid JSON";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }

    const char* url = ddoc["url"] | "";
    int intervalMin = ddoc["interval_min"] | 5;

    if (strlen(url) == 0) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "url is required";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_BAD_REQUEST, "application/json", json);
        return;
    }
    if (intervalMin < 1) {
        intervalMin = 1;
    }

    configManager.setLocketUrl(url);
    configManager.setLocketIntervalMin(intervalMin);

    if (!configManager.save()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["message"] = "Failed to save config";
        String json;
        serializeJson(doc, json);
        setCorsHeaders(webserver);
        webserver->raw().send(HTTP_CODE_INTERNAL_ERROR, "application/json", json);
        return;
    }

    // Trigger an immediate fetch so the user sees the result right away.
    bool ok = ImageFetcher::fetchAndDisplayNow();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["url"] = url;
    doc["interval_min"] = intervalMin;
    doc["fetched"] = ok;
    doc["lastStatus"] = ImageFetcher::lastStatus();
    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);

    Logger::info("Locket config updated", "API");
}
```

> ⚠️ `fetchAndDisplayNow()` ใน handler จะ block จนกว่า download+decode เสร็จ (อาจหลายวินาที). ภายในมี `wdtFeed()` กัน watchdog แล้ว. ถ้ารูปใหญ่มากและกลัว HTTP client ฝั่ง browser timeout ให้พิจารณา **ไม่** trigger ทันที แต่ตั้ง flag ให้ `loop()` fetch รอบถัดไปแทน (set `firstRunPending_ = true` ผ่าน method ใหม่). เวอร์ชันนี้เลือก trigger ทันทีเพื่อ UX ที่ดีกว่า.

### 4.5 `include/web/Api.h`

เพิ่ม declarations (กลุ่ม NTP/Display):
```cpp
void handleLocketConfigGet(Webserver* webserver);
void handleLocketConfigSet(Webserver* webserver);
```

### 4.6 `platformio.ini`

เพิ่มใน `lib_deps`:
```ini
lib_deps =
    bblanchon/ArduinoJson@^7.4.2
    moononournation/GFX Library for Arduino@^1.6.4
    bitbank2/AnimatedGIF@^2.2.0
    bitbank2/JPEGDEC@^1.6.2
```

> เช็ค version ล่าสุดด้วย `pio pkg search "JPEGDEC"`. ถ้ามีปัญหา flash size ให้ดู `.pio/build/esp12e/` หลัง build แล้วยืนยันว่ายังพอกับ partition.

### 4.7 Navigation link

> หมายเหตุสำคัญ: nav link **ไม่ได้อยู่ใน `data/web/header.html`** (header.html มีแค่ title + theme switch). nav links จริงอยู่ใน `data/web/index.html` ในบล็อก `<section>` (บรรทัด ~23-47).

แก้ `data/web/index.html` เพิ่มปุ่มในกลุ่ม nav (ต่อจากปุ่ม Logs):
```html
          <a href="./locket.html">
            <button type="button" class="secondary">Locket Image</button>
          </a>
```

---

## 5. ไฟล์ Web UI ที่ต้องสร้าง

### 5.1 `data/web/locket.html`

> โครงตาม `ntp.html` เป๊ะ ๆ: ใช้ `apiFetch` (จาก utils.js), `<header id="header-placeholder">`, back link, และ `main.js` โหลด header/footer.

```html
<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Locket Image</title>
    <link rel="stylesheet" href="./css/pico.min.css" />
    <link rel="stylesheet" href="./css/style.css" />
    <script src="./js/alpinejs.min.js" defer></script>
    <script src="./js/utils.js"></script>
    <script src="./js/themeSwitcher.js"></script>
    <script src="./js/locketHandler.js"></script>
    <script src="./js/main.js"></script>
  </head>
  <body>
    <main class="container">
      <header id="header-placeholder"></header>

      <a href="./index.html" class="wifi-back-link">← Back to Home</a>

      <section x-data="locketHandler()" x-init="init()">
        <h2>Locket Image</h2>
        <p>
          <small>
            แสดงรูป JPEG จาก URL บนจอ ทุก ๆ N นาที (รูปควร ≤ 240x240, JPEG only).
          </small>
        </p>

        <label for="locket-url">Image URL (JPEG)</label>
        <input
          id="locket-url"
          type="url"
          x-model="url"
          placeholder="https://example.com/photo.jpg"
        />

        <label for="locket-interval">Refetch Interval (minutes)</label>
        <input
          id="locket-interval"
          type="number"
          min="1"
          x-model.number="intervalMin"
          placeholder="5"
        />

        <div style="margin-top: 0.75em; display: flex; gap: 0.5em">
          <button
            type="button"
            class="primary"
            @click="save()"
            x-bind:disabled="loading"
            x-text="loading ? 'Fetching...' : 'Save & Fetch'"
          ></button>
          <button type="button" class="secondary" @click="fetchConfig()">
            Refresh
          </button>
        </div>

        <p style="margin-top: 1em">
          <strong>Status:</strong>
          <span x-text="lastStatus"></span>
        </p>
        <p>
          <small
            x-text="lastOk ? 'Last fetch succeeded' : 'Last fetch failed or not run yet'"
          ></small>
        </p>
      </section>

      <footer id="footer-placeholder"></footer>
    </main>
  </body>
</html>
```

### 5.2 `data/web/js/locketHandler.js`

> ใช้ `apiFetch` (ชื่อจริงใน utils.js — **ไม่ใช่** `fetchWithAuth`).

```javascript
function locketHandler() {
  return {
    loading: false,
    url: "",
    intervalMin: 5,
    lastStatus: "",
    lastOk: false,

    fetchConfig() {
      apiFetch("/api/v1/locket/config")
        .then((r) => r.json())
        .then((data) => {
          this.url = data.url || "";
          this.intervalMin = data.interval_min || 5;
          this.lastStatus = data.lastStatus || "";
          this.lastOk = data.lastOk || false;
        })
        .catch((err) => {
          this.lastStatus = "error fetching config";
          console.error(err);
        });
    },

    save() {
      if (!this.url) {
        this.lastStatus = "URL is required";
        return;
      }
      this.loading = true;
      const payload = {
        url: this.url,
        interval_min: Number(this.intervalMin) || 5,
      };
      apiFetch("/api/v1/locket/config", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      })
        .then((r) => r.json())
        .then((data) => {
          if (data.status === "ok") {
            this.lastStatus = data.fetched
              ? "Saved & image displayed"
              : "Saved, but fetch failed: " + (data.lastStatus || "");
            this.lastOk = !!data.fetched;
          } else {
            this.lastStatus = data.message || "save failed";
            this.lastOk = false;
          }
        })
        .catch((err) => {
          this.lastStatus = "save failed";
          this.lastOk = false;
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    init() {
      this.fetchConfig();
    },
  };
}
```

---

## 6. Build & Flash Steps

```bash
# 0. ติดตั้ง dependency ใหม่ (PlatformIO จะดึง JPEGDEC อัตโนมัติตอน build แรก)
pio pkg install

# 1. Build firmware
pio run -e esp12e

# 2. Build + upload filesystem (data/ -> LittleFS) — ต้องทำเพราะมีไฟล์ web ใหม่
pio run -e esp12e -t buildfs
pio run -e esp12e -t uploadfs

# 3. Flash firmware ผ่าน USB
pio run -e esp12e -t upload

# 4. Serial monitor ดู log
pio device monitor -b 115200
```

OTA (เมื่อบอร์ดออนไลน์แล้ว ใช้ web UI หรือ `scripts/flash-ota.sh`):
- firmware → POST `/api/v1/ota/fw`
- filesystem → POST `/api/v1/ota/fs`
ต้อง upload **ทั้งสอง** เพราะแก้ทั้ง C++ และไฟล์ใน `data/`.

ทดสอบ:
1. เปิด web UI → เมนู "Locket Image"
2. กรอก URL JPEG (เริ่มด้วย http เพื่อลด RAM ก่อน แล้วค่อยลอง https) + interval
3. กด Save & Fetch → ดูจอบอร์ดว่ารูปขึ้น และ Serial log `Locket displayed, free heap: ...`

---

## 7. ข้อควรระวัง (ESP8266)

### Memory
- **อย่าโหลดทั้งไฟล์เข้า RAM** — download stream → LittleFS, decode block-by-block ผ่าน callback. peak RAM = TLS buffer + 1 chunk + 1 MCU block เท่านั้น
- **HTTPS กิน RAM มาก** — `WiFiClientSecure.setBufferSizes(512, 512)` + `setInsecure()`. ถ้า heap ต่ำกว่า ~20KB ตอน fetch อาจ crash → log free heap ก่อน/หลัง และแนะนำ user ใช้ http หรือรูปเล็ก
- หลีกเลี่ยง `String` concatenation เยอะ ๆ ใน hot path (heap fragmentation)
- ตรวจ flash: `/cache/locket.jpg` + GIF + web UI ต้องไม่เกิน partition 2MB

### Watchdog (`WDTO_2S`)
- ทุก loop ของ download และทุก JPEGDEC draw callback เรียก `EspClass::wdtFeed()` + `yield()`
- ถ้า decode รูปใหญ่/progressive นานเกิน 2s ต่อ block → พิจารณา `JPEG_SCALE_HALF`
- handler `fetchAndDisplayNow()` ที่ block ใน HTTP request: ภายในมี wdtFeed แล้ว แต่ถ้าจอ/UX สำคัญกว่า ให้เปลี่ยนเป็น deferred-fetch (ตั้ง flag ให้ `loop()` ทำ) เพื่อให้ตอบ HTTP กลับเร็ว

### Error handling
- HTTP code != 200 → set `lastStatus` = `"http <code>"`, ไม่แตะรูปเดิมบนจอ
- download ได้ 0 bytes / decode fail → ลบ `.tmp`, คงรูป cache เดิมไว้ (ไม่ clear จอถ้า decode ใหม่ fail; `clearScreen()` ถูกเรียก **เฉพาะตอนเริ่ม decode สำเร็จระดับ open** — ถ้าอยากให้ปลอดภัยกว่า ย้าย `clearScreen()` ไปหลัง `jpeg.decode()` คืน rc==1 ไม่ได้เพราะวาดระหว่าง decode; ทางเลือก: decode ลง offscreen ไม่คุ้มบน ESP8266 → ยอมรับว่าจอจะว่างชั่วครู่ถ้า decode fail กลางคัน)
- WiFi หลุด → skip fetch, log warn, ลองใหม่รอบ interval ถัดไป
- URL ผิด scheme / parse fail → `http.begin` คืน false → `lastStatus = "http begin failed"`
- non-JPEG (เช่น server ส่ง HTML 404 page) → `jpeg.open` fail → `lastStatus = "jpeg open failed"`

### อื่น ๆ
- `millis()` overflow (~49.7 วัน) — การเปรียบเทียบ `now - lastFetchMs_ >= interval` ใช้ unsigned arithmetic จึงปลอดภัยอยู่แล้ว
- ตั้ง interval floor 1 นาที (UI + firmware) กัน user ตั้ง 0 แล้ว fetch รัว ๆ จน flash สึก/network spam
- JPEGDEC `open(File&, ...)` signature ต่างกันตามเวอร์ชัน → ยืนยันกับ example ของ lib หลัง `pio pkg install` (ดู §3.2 หมายเหตุ)
```

---

## 8. UI Redesign — ย้ายจาก Pico.css → Bootstrap 5

### 8.1 เป้าหมาย

แทนที่ Pico.css ด้วย **Bootstrap 5.3** เพื่อให้ได้:
- Layout สวยงาม มี navbar + sidebar card dashboard
- Dark/Light mode ผ่าน `data-bs-theme` (built-in Bootstrap 5.3) แทน custom CSS
- Responsive grid (`col-md-*`) รองรับทั้ง desktop และ mobile
- Bootstrap component ครบ: badge, card, alert, spinner, toast, table, modal
- ยังคงใช้ **Alpine.js** ควบคู่ (Alpine + BS5 compatible เพราะ BS5 ไม่ใช้ jQuery)

### 8.2 LittleFS Size Analysis

| ไฟล์ | ขนาดโดยประมาณ | action |
|------|--------------|--------|
| `css/pico.min.css` | ~15 KB | **ลบทิ้ง** |
| `css/bootstrap.min.css` | ~160 KB | **เพิ่มใหม่** |
| `js/bootstrap.bundle.min.js` | ~60 KB | **เพิ่มใหม่** (รวม Popper แล้ว) |
| `js/alpinejs.min.js` | ~44 KB | คงไว้ |

เพิ่มสุทธิ **+205 KB** — LittleFS partition ขนาด 2 MB รับได้สบาย

> ไม่แนะนำใช้ CDN เพราะ user อาจ config อุปกรณ์บน local network ที่ไม่มี internet

### 8.3 ดาวน์โหลด Bootstrap

```bash
# ดาวน์โหลดไฟล์เข้า data/web/
curl -L https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css \
     -o data/web/css/bootstrap.min.css

curl -L https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js \
     -o data/web/js/bootstrap.bundle.min.js

# ลบ Pico.css
rm data/web/css/pico.min.css
```

### 8.4 ไฟล์ที่ต้องแก้ไข

#### 8.4.1 `data/web/css/style.css` — แก้ให้ทำงานกับ Bootstrap

ลบ class ที่ทับซ้อนกับ Bootstrap และปรับ custom style ที่ยังต้องการ:

```css
/* ลบทิ้ง: .wifi-back-link, .wifi-section, .wifi-connect, .password-row,
            .action-row, .scan-table-container, .wifi-table, .ssid-cell,
            .signal-cell, .scan-btn, .no-networks (Bootstrap จัดการแทน)
   คงไว้เฉพาะ: */

/* Theme switcher button */
.theme-switch-btn {
  background: none;
  border: none;
  cursor: pointer;
  padding: 0.25rem;
  line-height: 0;
}

/* Water drop overlay animation */
#theme-waterdrop-overlay {
  position: fixed;
  inset: 0;
  pointer-events: none;
  z-index: 9999;
  background: var(--bs-body-bg);
  clip-path: circle(0% at 50% 50%);
  transition: clip-path 0.7s cubic-bezier(0.4, 0, 0.2, 1);
}
#theme-waterdrop-overlay.active {
  pointer-events: auto;
}

/* Sidebar active link */
.sidebar .nav-link.active {
  background-color: var(--bs-primary);
  color: #fff;
  border-radius: 0.375rem;
}
.sidebar .nav-link {
  color: var(--bs-body-color);
  padding: 0.5rem 1rem;
  border-radius: 0.375rem;
  transition: background 0.15s;
}
.sidebar .nav-link:hover {
  background-color: var(--bs-secondary-bg);
}
```

#### 8.4.2 `data/web/js/themeSwitcher.js` — ปรับให้ใช้ `data-bs-theme`

```javascript
function themeSwitcher() {
  return {
    isDark: false,

    init() {
      const saved = localStorage.getItem('theme') || 'light';
      this.isDark = saved === 'dark';
      this._apply(saved);
    },

    toggleTheme() {
      this.isDark = !this.isDark;
      const theme = this.isDark ? 'dark' : 'light';
      localStorage.setItem('theme', theme);
      this._applyWithAnimation(theme);
    },

    _apply(theme) {
      document.documentElement.setAttribute('data-bs-theme', theme);
    },

    _applyWithAnimation(theme) {
      const overlay = document.getElementById('theme-waterdrop-overlay');
      overlay.classList.add('active');
      overlay.style.clipPath = 'circle(150% at 50% 50%)';
      setTimeout(() => {
        this._apply(theme);
        overlay.style.clipPath = 'circle(0% at 50% 50%)';
        setTimeout(() => overlay.classList.remove('active'), 700);
      }, 50);
    }
  };
}
```

#### 8.4.3 `data/web/header.html` — Bootstrap Navbar

```html
<nav class="navbar navbar-expand-md bg-body-tertiary border-bottom px-3" x-data="themeSwitcher()" x-init="init()">
  <a class="navbar-brand fw-bold" href="./index.html">
    <svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor" class="me-1">
      <rect x="2" y="2" width="9" height="9" rx="1"/>
      <rect x="13" y="2" width="9" height="9" rx="1"/>
      <rect x="2" y="13" width="9" height="9" rx="1"/>
      <rect x="13" y="13" width="9" height="9" rx="1"/>
    </svg>
    GeekMagic
  </a>
  <button class="navbar-toggler" type="button" data-bs-toggle="collapse" data-bs-target="#navMenu">
    <span class="navbar-toggler-icon"></span>
  </button>
  <div class="collapse navbar-collapse" id="navMenu">
    <ul class="navbar-nav me-auto mb-2 mb-md-0">
      <li class="nav-item"><a class="nav-link" href="./wifi.html">WiFi</a></li>
      <li class="nav-item"><a class="nav-link" href="./ntp.html">NTP</a></li>
      <li class="nav-item"><a class="nav-link" href="./rotation.html">Rotation</a></li>
      <li class="nav-item"><a class="nav-link" href="./update.html">OTA Update</a></li>
      <li class="nav-item"><a class="nav-link" href="./gif_upload.html">GIF</a></li>
      <li class="nav-item"><a class="nav-link" href="./locket.html">Locket</a></li>
      <li class="nav-item"><a class="nav-link" href="./token.html">Token</a></li>
      <li class="nav-item"><a class="nav-link" href="./logs.html">Logs</a></li>
    </ul>
    <button class="theme-switch-btn ms-2" @click="toggleTheme()" aria-label="Toggle theme">
      <!-- Sun icon (light mode) -->
      <svg x-show="isDark" width="20" height="20" viewBox="0 0 24 24" fill="none"
           stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <circle cx="12" cy="12" r="5"/>
        <line x1="12" y1="1" x2="12" y2="3"/>
        <line x1="12" y1="21" x2="12" y2="23"/>
        <line x1="4.22" y1="4.22" x2="5.64" y2="5.64"/>
        <line x1="18.36" y1="18.36" x2="19.78" y2="19.78"/>
        <line x1="1" y1="12" x2="3" y2="12"/>
        <line x1="21" y1="12" x2="23" y2="12"/>
        <line x1="4.22" y1="19.78" x2="5.64" y2="18.36"/>
        <line x1="18.36" y1="5.64" x2="19.78" y2="4.22"/>
      </svg>
      <!-- Moon icon (dark mode) -->
      <svg x-show="!isDark" width="20" height="20" viewBox="0 0 24 24" fill="none"
           stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/>
      </svg>
    </button>
  </div>
</nav>
<div id="theme-waterdrop-overlay"></div>
```

#### 8.4.4 `data/web/footer.html` — Bootstrap footer

```html
<footer class="border-top py-3 mt-auto text-center text-body-secondary small">
  <a href="https://github.com/Times-Z/GeekMagic-Open-Firmware" target="_blank" class="text-decoration-none">
    GeekMagic Open Firmware
  </a>
</footer>
```

#### 8.4.5 `data/web/index.html` — Dashboard Card Grid

แทนที่ปุ่ม list เป็น card grid แบบ Bootstrap:

```html
<!doctype html>
<html lang="en">
<head>
  <meta charset="UTF-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>GeekMagic Open Firmware</title>
  <link rel="stylesheet" href="./css/bootstrap.min.css"/>
  <link rel="stylesheet" href="./css/style.css"/>
  <script src="./js/bootstrap.bundle.min.js"></script>
  <script src="./js/alpinejs.min.js" defer></script>
  <script src="./js/utils.js"></script>
  <script src="./js/themeSwitcher.js"></script>
  <script src="./js/rebootHandler.js"></script>
  <script src="./js/main.js"></script>
</head>
<body class="d-flex flex-column min-vh-100">
  <header id="header-placeholder"></header>

  <main class="container py-4 flex-grow-1">
    <h2 class="mb-4">Dashboard</h2>

    <div class="row g-3">
      <!-- WiFi -->
      <div class="col-6 col-md-4 col-lg-3">
        <a href="./wifi.html" class="text-decoration-none">
          <div class="card h-100 text-center p-3 shadow-sm">
            <div class="fs-1">📶</div>
            <div class="mt-2 fw-semibold">WiFi</div>
          </div>
        </a>
      </div>
      <!-- NTP -->
      <div class="col-6 col-md-4 col-lg-3">
        <a href="./ntp.html" class="text-decoration-none">
          <div class="card h-100 text-center p-3 shadow-sm">
            <div class="fs-1">🕐</div>
            <div class="mt-2 fw-semibold">Time (NTP)</div>
          </div>
        </a>
      </div>
      <!-- Screen Rotation -->
      <div class="col-6 col-md-4 col-lg-3">
        <a href="./rotation.html" class="text-decoration-none">
          <div class="card h-100 text-center p-3 shadow-sm">
            <div class="fs-1">🔄</div>
            <div class="mt-2 fw-semibold">Screen Rotation</div>
          </div>
        </a>
      </div>
      <!-- OTA Update -->
      <div class="col-6 col-md-4 col-lg-3">
        <a href="./update.html" class="text-decoration-none">
          <div class="card h-100 text-center p-3 shadow-sm">
            <div class="fs-1">⬆️</div>
            <div class="mt-2 fw-semibold">OTA Update</div>
          </div>
        </a>
      </div>
      <!-- GIF Upload -->
      <div class="col-6 col-md-4 col-lg-3">
        <a href="./gif_upload.html" class="text-decoration-none">
          <div class="card h-100 text-center p-3 shadow-sm">
            <div class="fs-1">🎞️</div>
            <div class="mt-2 fw-semibold">Upload GIF</div>
          </div>
        </a>
      </div>
      <!-- Locket -->
      <div class="col-6 col-md-4 col-lg-3">
        <a href="./locket.html" class="text-decoration-none">
          <div class="card h-100 text-center p-3 shadow-sm border-primary">
            <div class="fs-1">🖼️</div>
            <div class="mt-2 fw-semibold text-primary">Locket</div>
          </div>
        </a>
      </div>
      <!-- API Token -->
      <div class="col-6 col-md-4 col-lg-3">
        <a href="./token.html" class="text-decoration-none">
          <div class="card h-100 text-center p-3 shadow-sm">
            <div class="fs-1">🔑</div>
            <div class="mt-2 fw-semibold">API Token</div>
          </div>
        </a>
      </div>
      <!-- Logs -->
      <div class="col-6 col-md-4 col-lg-3">
        <a href="./logs.html" class="text-decoration-none">
          <div class="card h-100 text-center p-3 shadow-sm">
            <div class="fs-1">📋</div>
            <div class="mt-2 fw-semibold">Logs</div>
          </div>
        </a>
      </div>
    </div>

    <!-- Reboot -->
    <div class="mt-4" x-data="rebootHandler()">
      <button class="btn btn-danger" x-bind:disabled="loading" x-on:click="reboot">
        <span x-show="loading" class="spinner-border spinner-border-sm me-1" role="status"></span>
        Reboot Device
      </button>
      <div x-show="message" class="alert alert-warning mt-2" x-text="message"></div>
    </div>
  </main>

  <footer id="footer-placeholder"></footer>
</body>
</html>
```

#### 8.4.6 `data/web/locket.html` — แก้ให้ใช้ Bootstrap (แทนที่ version ใน §5.1)

```html
<!doctype html>
<html lang="en">
<head>
  <meta charset="UTF-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>Locket</title>
  <link rel="stylesheet" href="./css/bootstrap.min.css"/>
  <link rel="stylesheet" href="./css/style.css"/>
  <script src="./js/bootstrap.bundle.min.js"></script>
  <script src="./js/alpinejs.min.js" defer></script>
  <script src="./js/utils.js"></script>
  <script src="./js/themeSwitcher.js"></script>
  <script src="./js/locketHandler.js"></script>
  <script src="./js/main.js"></script>
</head>
<body class="d-flex flex-column min-vh-100">
  <header id="header-placeholder"></header>

  <main class="container py-4 flex-grow-1">
    <nav aria-label="breadcrumb" class="mb-3">
      <ol class="breadcrumb">
        <li class="breadcrumb-item"><a href="./index.html">Home</a></li>
        <li class="breadcrumb-item active">Locket</li>
      </ol>
    </nav>

    <div class="row justify-content-center">
      <div class="col-12 col-md-8 col-lg-6">
        <div class="card shadow-sm" x-data="locketHandler()" x-init="init()">
          <div class="card-header d-flex align-items-center gap-2">
            <span class="fs-5">🖼️</span>
            <strong>Image Display (Locket)</strong>
            <!-- Status badge -->
            <span class="badge ms-auto"
              :class="status === 'ok' ? 'bg-success' : status === 'error' ? 'bg-danger' : 'bg-secondary'"
              x-text="status || 'idle'">
            </span>
          </div>
          <div class="card-body">

            <!-- URL -->
            <div class="mb-3">
              <label for="locket-url" class="form-label fw-semibold">Image URL <span class="text-danger">*</span></label>
              <input id="locket-url" type="url" class="form-control"
                     placeholder="http://example.com/photo.jpg"
                     x-model="url"/>
              <div class="form-text">JPEG เท่านั้น ขนาดแนะนำ ≤ 240×240 px, ≤ 200 KB</div>
            </div>

            <!-- Interval -->
            <div class="mb-3">
              <label for="locket-interval" class="form-label fw-semibold">Refetch Interval (นาที)</label>
              <div class="input-group" style="max-width: 180px">
                <input id="locket-interval" type="number" class="form-control"
                       min="1" max="1440" x-model.number="intervalMin"/>
                <span class="input-group-text">min</span>
              </div>
              <div class="form-text">ขั้นต่ำ 1 นาที</div>
            </div>

            <!-- Actions -->
            <div class="d-flex gap-2 flex-wrap">
              <button class="btn btn-primary" @click="save()" :disabled="saving">
                <span x-show="saving" class="spinner-border spinner-border-sm me-1"></span>
                Save & Fetch
              </button>
              <button class="btn btn-outline-secondary" @click="fetchNow()" :disabled="fetching">
                <span x-show="fetching" class="spinner-border spinner-border-sm me-1"></span>
                Fetch Now
              </button>
            </div>

            <!-- Alert -->
            <div x-show="message" class="mt-3 alert"
                 :class="status === 'error' ? 'alert-danger' : 'alert-success'"
                 x-text="message">
            </div>

            <!-- Info: last fetch -->
            <div x-show="lastFetchTime" class="mt-3 text-body-secondary small">
              Last fetch: <span x-text="lastFetchTime"></span>
              &nbsp;|&nbsp;
              Next in: <strong x-text="nextFetchCountdown"></strong>
            </div>

          </div>
        </div>
      </div>
    </div>
  </main>

  <footer id="footer-placeholder"></footer>
</body>
</html>
```

#### 8.4.7 ตัวอย่าง `data/web/wifi.html` — แก้เป็น Bootstrap (เป็น reference pattern สำหรับหน้าอื่น)

```html
<!doctype html>
<html lang="en">
<head>
  <meta charset="UTF-8"/>
  <meta name="viewport" content="width=device-width, initial-scale=1"/>
  <title>WiFi</title>
  <link rel="stylesheet" href="./css/bootstrap.min.css"/>
  <link rel="stylesheet" href="./css/style.css"/>
  <script src="./js/bootstrap.bundle.min.js"></script>
  <script src="./js/alpinejs.min.js" defer></script>
  <script src="./js/utils.js"></script>
  <script src="./js/themeSwitcher.js"></script>
  <script src="./js/wifiHandler.js"></script>
  <script src="./js/main.js"></script>
</head>
<body class="d-flex flex-column min-vh-100">
  <header id="header-placeholder"></header>

  <main class="container py-4 flex-grow-1">
    <nav aria-label="breadcrumb" class="mb-3">
      <ol class="breadcrumb">
        <li class="breadcrumb-item"><a href="./index.html">Home</a></li>
        <li class="breadcrumb-item active">WiFi</li>
      </ol>
    </nav>

    <div class="row g-4" x-data="wifiHandler()" x-init="init()">

      <!-- Connect form -->
      <div class="col-12 col-md-5">
        <div class="card shadow-sm h-100">
          <div class="card-header fw-semibold">📶 Connect to Network</div>
          <div class="card-body">
            <form @submit.prevent="connect()">
              <div class="mb-3">
                <label for="ssid" class="form-label">SSID</label>
                <input id="ssid" type="text" class="form-control" x-model="ssid" autocomplete="off"/>
              </div>
              <div class="mb-3">
                <label for="password" class="form-label">Password</label>
                <div class="input-group">
                  <input id="password" class="form-control" x-model="password"
                         :type="showPassword ? 'text' : 'password'"/>
                  <button class="btn btn-outline-secondary" type="button"
                          @click="showPassword = !showPassword">
                    <span x-text="showPassword ? '🙈' : '👁'"></span>
                  </button>
                </div>
              </div>
              <div class="d-flex gap-2">
                <button type="submit" class="btn btn-primary" :disabled="connecting">
                  <span x-show="connecting" class="spinner-border spinner-border-sm me-1"></span>
                  Save & Connect
                </button>
                <button type="button" class="btn btn-outline-danger" @click="forget()">Forget</button>
              </div>
              <div x-show="statusMsg" class="mt-2 alert"
                   :class="statusMsg.includes('error') || statusMsg.includes('fail') ? 'alert-danger' : 'alert-success'"
                   x-text="statusMsg">
              </div>
            </form>
          </div>
        </div>
      </div>

      <!-- Scan results -->
      <div class="col-12 col-md-7">
        <div class="card shadow-sm h-100">
          <div class="card-header d-flex align-items-center gap-2">
            <span class="fw-semibold">Available Networks</span>
            <button class="btn btn-sm btn-outline-primary ms-auto" @click="scan()" :disabled="scanning">
              <span x-show="scanning" class="spinner-border spinner-border-sm me-1"></span>
              Scan
            </button>
          </div>
          <div class="card-body p-0">
            <table class="table table-hover table-sm mb-0">
              <thead class="table-light">
                <tr>
                  <th class="ps-3">SSID</th>
                  <th>Signal</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                <template x-for="net in networks" :key="net.ssid">
                  <tr>
                    <td class="ps-3 fw-semibold" x-text="net.ssid"></td>
                    <td>
                      <span x-text="net.rssiDisplay"></span>
                      <span class="text-muted font-monospace ms-1" x-text="net.bars"></span>
                    </td>
                    <td>
                      <button class="btn btn-sm btn-outline-secondary" @click="selectNetwork(net)">Use</button>
                    </td>
                  </tr>
                </template>
                <tr x-show="networks.length === 0 && !scanning">
                  <td colspan="3" class="text-center text-muted py-3">
                    <em>No networks found — click Scan</em>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  </main>

  <footer id="footer-placeholder"></footer>
</body>
</html>
```

### 8.5 Class Mapping — Pico.css → Bootstrap 5

| Pico.css / เดิม | Bootstrap 5 |
|-----------------|-------------|
| `<button>` (primary) | `<button class="btn btn-primary">` |
| `<button class="secondary">` | `<button class="btn btn-secondary">` |
| `<button class="contrast">` | `<button class="btn btn-danger">` |
| `<main class="container">` | `<main class="container py-4">` |
| `<input type="text">` | `<input type="text" class="form-control">` |
| `<label>` | `<label class="form-label">` |
| `[data-theme="dark"]` | `[data-bs-theme="dark"]` |
| ไม่มี spinner | `<span class="spinner-border spinner-border-sm">` |
| ไม่มี toast | Bootstrap Toast component |
| ไม่มี breadcrumb | `<ol class="breadcrumb">` |

### 8.6 Pattern ที่ต้อง apply กับทุกหน้า

หน้าที่เหลือ (`ntp.html`, `rotation.html`, `update.html`, `gif_upload.html`, `token.html`, `logs.html`) ให้ทำตาม pattern เดียวกับ `wifi.html`:

1. แทนที่ `pico.min.css` → `bootstrap.min.css` + เพิ่ม `bootstrap.bundle.min.js`
2. เพิ่ม `class="d-flex flex-column min-vh-100"` ที่ `<body>`
3. แทนที่ link "← Back to Home" ด้วย Bootstrap breadcrumb
4. ห่อ section ด้วย Bootstrap `card`
5. แทนที่ `<button>` ด้วย `btn btn-*`
6. แทนที่ `<input>` ด้วย `form-control`
7. แทนที่ `<p x-text="msg">` ด้วย Bootstrap `alert`

### 8.7 ลำดับการทำงาน (Build & Flash)

```bash
# 1. ดาวน์โหลด Bootstrap
curl -L https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css \
     -o data/web/css/bootstrap.min.css
curl -L https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js \
     -o data/web/js/bootstrap.bundle.min.js

# 2. ลบ pico.min.css
rm data/web/css/pico.min.css

# 3. แก้ไขไฟล์ HTML/CSS/JS ตาม plan นี้

# 4. Build LittleFS เท่านั้น (firmware ไม่ต้อง build ใหม่)
pio run --target buildfs

# 5. Flash filesystem ผ่าน OTA
bash scripts/flash-ota.sh <device-ip> --fs-only
# หรือ
curl -X POST http://<device-ip>/api/v1/ota/fs \
     -H "Authorization: Bearer <token>" \
     -F "file=@.pio/build/esp12e/littlefs.bin"
```

> **หมายเหตุ:** การเปลี่ยน CSS framework กระทบเฉพาะ `data/` (LittleFS) เท่านั้น — ไม่แตะ C++ firmware เลย ไม่ต้อง `pio run` ใหม่
