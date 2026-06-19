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

unsigned long ImageFetcher::lastFetchMs_ = 0;
bool ImageFetcher::firstRunPending_ = false;
String ImageFetcher::lastStatus_ = "idle";
bool ImageFetcher::lastOk_ = false;

namespace {
constexpr size_t DL_CHUNK = 512;
constexpr unsigned long MIN_INTERVAL_MS = 60UL * 1000UL;
constexpr int HTTP_TIMEOUT_MS = 15000;
constexpr int16_t SCREEN_W = 240;
constexpr int16_t SCREEN_H = 240;

JPEGDEC jpeg;

auto jpegDrawCallback(JPEGDRAW* pDraw) -> int {
    Arduino_GFX* gfx = DisplayManager::getGfx();
    if (gfx == nullptr) {
        return 0;
    }
    if (pDraw->x >= SCREEN_W || pDraw->y >= SCREEN_H) {
        return 1;
    }
    gfx->draw16bitRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    EspClass::wdtFeed();
    return 1;
}
}  // namespace

auto ImageFetcher::begin() -> void {
    lastFetchMs_ = 0;
    firstRunPending_ = (strlen(configManager.getLocketUrl()) > 0);
    lastStatus_ = "ready";
    if (!LittleFS.exists("/cache")) {
        LittleFS.mkdir("/cache");
    }
}

auto ImageFetcher::loop() -> void {
    const char* url = configManager.getLocketUrl();
    if (url == nullptr || strlen(url) == 0) {
        return;
    }

    unsigned long intervalMs =
        static_cast<unsigned long>(configManager.getLocketIntervalMin()) * 60UL * 1000UL;
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

auto ImageFetcher::fetchAndDisplayNow() -> bool {
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
        return false;
    }

    if (!decodeAndDraw()) {
        lastStatus_ = "decode failed";
        lastOk_ = false;
        Logger::error("Locket JPEG decode failed", "Locket");
        return false;
    }

    lastStatus_ = "ok";
    lastOk_ = true;
    Logger::info(
        (String("Locket displayed, free heap: ") + String(ESP.getFreeHeap())).c_str(),  // NOLINT
        "Locket");
    return true;
}

auto ImageFetcher::downloadToCache(const String& url) -> bool {
    std::unique_ptr<WiFiClient> client;
    if (url.startsWith("https://")) {
        auto* secure = new WiFiClientSecure();
        secure->setInsecure();
        secure->setBufferSizes(512, 512);
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
    int contentLen = http.getSize();
    size_t written = 0;

    while (http.connected() && (contentLen > 0 || contentLen == -1)) {
        size_t avail = stream->available();
        if (avail > 0) {
            int toRead = (avail > sizeof(buf)) ? static_cast<int>(sizeof(buf)) : static_cast<int>(avail);
            int n = stream->readBytes(buf, toRead);
            if (n <= 0) {
                break;
            }
            out.write(buf, n);
            written += static_cast<size_t>(n);
            if (contentLen > 0) {
                contentLen -= n;
            }
        } else {
            if (contentLen == -1 && !stream->connected()) {
                break;
            }
            delay(1);
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

    LittleFS.remove(CACHE_PATH);
    if (!LittleFS.rename(CACHE_TMP_PATH, CACHE_PATH)) {
        LittleFS.remove(CACHE_TMP_PATH);
        lastStatus_ = "cache rename failed";
        return false;
    }

    Logger::info(
        (String("Locket downloaded ") + String(written) + " bytes").c_str(), "Locket");
    return true;
}

auto ImageFetcher::decodeAndDraw() -> bool {
    File f = LittleFS.open(CACHE_PATH, "r");
    if (!f) {
        lastStatus_ = "cache read failed";
        return false;
    }

    if (!jpeg.open(f, jpegDrawCallback)) {
        f.close();
        lastStatus_ = "jpeg open failed";
        return false;
    }
    jpeg.setPixelType(RGB565_BIG_ENDIAN);

    DisplayManager::clearScreen();

    int rc = jpeg.decode(0, 0, 0);
    jpeg.close();
    f.close();

    if (rc != 1) {
        lastStatus_ = "jpeg decode rc!=1";
        return false;
    }
    return true;
}

auto ImageFetcher::lastStatus() -> const String& { return lastStatus_; }
auto ImageFetcher::lastOk() -> bool { return lastOk_; }
