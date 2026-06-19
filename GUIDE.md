# คู่มือ Flash GeekMagic Ultra (Smalltv-Ultra)

**Board:** ESP8266EX (ESP8266MOD WiFi)  
**Flash size:** 4MB, mode: DIO

---

## วิธีที่ 1 — Flash ผ่าน WiFi (OTA) แนะนำ

ใช้เมื่ออุปกรณ์ยังเปิดติดและเชื่อมต่อ WiFi ได้ปกติ

### Flash Firmware

เปิด browser ไปที่:

```
http://<device-ip>/update
```

อัปโหลดไฟล์ `firmware.bin`

### Flash Filesystem

```bash
bash scripts/flash-ota.sh <device-ip> --fs-only
```

---

## วิธีที่ 2 — Flash ผ่าน USB

ใช้เมื่ออุปกรณ์เปิดไม่ติด จอดำ หรือเข้า WiFi ไม่ได้

### อุปกรณ์ที่ต้องใช้

- USB to TTL converter
- Jumper wire (หัวตัวผู้-ตัวเมีย)
- หัวแร้ง (สำหรับ soldering)

### การต่อสาย

ต่อสายเรียงจากบนลงล่างตามลำดับ (Soldering ให้เรียบร้อย):

| ลำดับ | ESP8266 | USB to TTL |
|-------|---------|------------|
| 1 | GND (วงพิเศษ บนสุด) | GND |
| 2 | RXD | TXD |
| 3 | TXD | RXD |
| 4 | VCC | ไม่ต่อ (จ่ายไฟผ่าน supply หลัก) |
| 5 | GPIO0 | GND (ต่อเฉพาะตอน Flash) ต่อสายให้ถอดเข้าออกได้ง่าย |

> GPIO0 ต่อลง GND เฉพาะตอนจะ Flash เสร็จแล้วให้ถอดออก

---

## ขั้นตอน Flash ผ่าน USB

### ขั้นที่ 1 — Build

```bash
# build firmware
/Users/peter/.platformio/penv/bin/platformio run

# build filesystem
/Users/peter/.platformio/penv/bin/platformio run --target buildfs
```

ไฟล์ที่ได้:

```
.pio/build/esp12e/firmware.bin
.pio/build/esp12e/littlefs.bin
```

### ขั้นที่ 2 — หา Serial Port

```bash
ls /dev/cu.* | grep -v Bluetooth
```

Port จะมีชื่อประมาณ `/dev/cu.usbserial-1110` และ **เปลี่ยนทุกครั้งที่เสียบ USB ใหม่**

เช็คว่าบอร์ดพร้อม (อยู่ใน flash mode):

```bash
python3 -m esptool --port /dev/cu.usbserial-1110 --baud 115200 chip_id
```

ถ้าได้ `Chip is ESP8266EX` แสดงว่าพร้อมแล้ว

> หมายเหตุ: `chip_id` จะ reset บอร์ดออกจาก flash mode อัตโนมัติหลังรันเสร็จ ต้องเข้า flash mode ใหม่ก่อน flash จริง

### ขั้นที่ 3 — Flash Firmware

```bash
python3 -m esptool \
  --port /dev/cu.usbserial-1110 \
  --baud 115200 \
  --chip esp8266 \
  write_flash \
  --flash_mode dio \
  --flash_size 4MB \
  0x0 .pio/build/esp12e/firmware.bin
```

สำเร็จเมื่อเห็น `Hash of data verified.`

### ขั้นที่ 4 — Flash Filesystem ผ่าน WiFi

หลัง flash firmware บอร์ดจะสร้าง WiFi AP:

- **SSID:** `GeekMagic`
- **Password:** `$str0ngPa$$w0rd`

เชื่อมต่อ WiFi แล้วรัน:

```bash
bash scripts/flash-ota.sh 192.168.4.1 --fs-only
```

หรือเปิด browser ไปที่ `http://192.168.4.1/legacyupdate` แล้วอัปโหลด `littlefs.bin` ด้วยตัวเอง

---

## Restore Backup Firmware (Rollback)

```bash
python3 -m esptool \
  --port /dev/cu.usbserial-1110 \
  --baud 115200 \
  --chip esp8266 \
  write_flash \
  --flash_mode dio \
  --flash_size 4MB \
  0x0 backup/Smalltv-Ultra/9.0.40_backup_full.bin
```
ต้องใช้ Version นี้ถึงจะ Flash ได้

---

## Tips

| ปัญหา | วิธีแก้ |
|-------|---------|
| `esptool` ไม่อยู่ใน PATH | ใช้ `python3 -m esptool` แทน |
| baud 921600 ขึ้น noise error | ลด baud เป็น `115200` |
| port หาย / `No such file or directory` | เสียบ USB ใหม่แล้ว `ls /dev/cu.*` อีกครั้ง |
| เชื่อมต่อไม่ได้ `No serial data received` | GPIO0 ยังไม่ได้ต่อลง GND หรือยังไม่ได้ reset บอร์ด |

---

# Developer Guide — สร้าง Software ของตัวเอง

สำหรับนักพัฒนาที่ต้องการเพิ่ม Feature ใหม่หรือสร้าง Firmware เวอร์ชันของตัวเอง

---

## ภาษาและ Stack ที่ใช้

| Layer | ภาษา / เทคโนโลยี | บทบาท |
|-------|------------------|--------|
| Firmware | **C++17** (Arduino Framework) | Logic หลัก, ควบคุม Hardware |
| Build System | **PlatformIO** (Python-based) | Build, Flash, Library management |
| Filesystem | **LittleFS** | เก็บ config, web UI, GIF files |
| Web UI (Frontend) | **HTML + Vanilla JS + Alpine.js** | หน้า Web จัดการอุปกรณ์ |
| Web UI (CSS) | **Pico.css** | Minimal CSS framework |
| API | **REST over HTTP** (JSON) | สื่อสารระหว่าง Web UI กับ Firmware |
| Config | **JSON** (ArduinoJson) | เก็บ config |
| CI/CD | **GitHub Actions** | Build และ Release อัตโนมัติ |

> ไม่มี RTOS — ใช้ Arduino single-threaded loop (`setup()` + `loop()`)

---

## โครงสร้างโปรเจกต์

```
.
├── src/                    # Source code หลัก (C++)
│   ├── main.cpp            # Entry point: setup() และ loop()
│   ├── display/
│   │   ├── DisplayManager.cpp   # จัดการหน้าจอ ST7789
│   │   └── Gif.cpp              # เล่น GIF บนจอ
│   ├── web/
│   │   ├── Webserver.cpp        # HTTP Server wrapper
│   │   └── Api.cpp              # REST API endpoints ทั้งหมด
│   ├── config/
│   │   ├── ConfigManager.cpp    # โหลด/บันทึก config.json
│   │   └── SecureStorage.cpp    # เก็บ secret ใน EEPROM (XOR + SHA-256)
│   ├── wireless/
│   │   └── WiFiManager.cpp      # เชื่อมต่อ WiFi / AP mode
│   ├── ntp/
│   │   └── NTPClient.cpp        # ซิงค์เวลาจาก NTP server
│   └── boot/
│       └── RescueMode.cpp       # Rescue mode เมื่อ boot loop
│
├── include/                # Header files (.h)
│   ├── display/DisplayManager.h
│   ├── web/Api.h
│   ├── config/ConfigManager.h
│   └── ...
│
├── data/                   # ไฟล์ที่จะถูก Flash ลง LittleFS
│   ├── config.json         # Config ตั้งต้น
│   ├── web/                # Web UI (HTML, CSS, JS)
│   │   ├── index.html
│   │   ├── css/
│   │   └── js/
│   └── gif/                # GIF ที่อัปโหลดไว้
│
├── platformio.ini          # Build configuration (target, libraries, flags)
├── scripts/
│   ├── build-with-docker.sh   # Build ด้วย Docker (ไม่ต้อง install PlatformIO)
│   ├── flash-ota.sh           # Flash ผ่าน WiFi OTA
│   └── git_version.py         # สร้าง project_version.h จาก git tag
└── swagger.yml             # OpenAPI spec (auto-generated)
```

---

## วิธีสร้าง Feature ใหม่ใน Firmware (C++)

### ขั้นที่ 1 — สร้าง Module ใหม่

สร้างไฟล์ `include/myfeature/MyFeature.h`:

```cpp
#pragma once
#include <Arduino.h>

class MyFeature {
public:
    static void begin();
    static void update();    // เรียกใน loop()
    static void doSomething();
};
```

สร้างไฟล์ `src/myfeature/MyFeature.cpp`:

```cpp
#include "myfeature/MyFeature.h"
#include <Logger.h>

void MyFeature::begin() {
    Logger::info("MyFeature initialized", "MyFeature");
}

void MyFeature::update() {
    // logic ที่ต้องรันทุก loop
}

void MyFeature::doSomething() {
    Logger::info("Doing something!", "MyFeature");
}
```

### ขั้นที่ 2 — เรียกใช้ใน main.cpp

```cpp
// src/main.cpp
#include "myfeature/MyFeature.h"

void setup() {
    // ... existing code ...
    MyFeature::begin();
}

void loop() {
    // ... existing code ...
    MyFeature::update();
}
```

### ขั้นที่ 3 — เพิ่ม API Endpoint (ถ้าต้องการ)

เพิ่มใน `src/web/Api.cpp` ภายใน `registerApiEndpoints()`:

```cpp
// @openapi {get} /myfeature/status version=v1 group=MyFeature summary="Get MyFeature status" requiresAuth=true
// responses=200:application/json,401:application/json
webserver->raw().on("/api/v1/myfeature/status", HTTP_GET, [webserver]() {
    if (!requireBearerToken(webserver)) return;

    JsonDocument doc;
    doc["status"] = "ok";
    doc["value"] = 42;

    String json;
    serializeJson(doc, json);

    setCorsHeaders(webserver);
    webserver->raw().send(HTTP_CODE_OK, "application/json", json);
});
```

เพิ่ม declaration ใน `include/web/Api.h`:

```cpp
void handleMyFeatureStatus(Webserver* webserver);
```

### หลักการสำคัญ

- **ห้าม block loop()** — ถ้ามี task ที่ใช้เวลานาน ให้ทำ state machine แทน
- **เรียก `yield()`** ใน loop ยาว ๆ เพื่อ feed watchdog และป้องกัน crash
- **ใช้ `Logger::info/warn/error()`** แทน `Serial.println()` — log จะถูกเก็บใน buffer และเข้าถึงได้ผ่าน API
- **Bearer Token** — ทุก endpoint ต้องเรียก `requireBearerToken()` ก่อน
- **ใช้ `configManager`** (global) เพื่อเข้าถึง config และ save

---

## วิธีสร้าง Feature บน Web UI (LittleFS)

Web UI อยู่ทั้งหมดใน `data/web/` — ไฟล์ทั้งหมดในโฟลเดอร์นี้จะถูก Flash ลง LittleFS

### โครงสร้าง Web UI

```
data/web/
├── index.html          # หน้าหลัก (dashboard)
├── wifi.html           # จัดการ WiFi
├── ntp.html            # ตั้ง NTP
├── rotation.html       # ปรับหน้าจอ
├── update.html         # OTA update
├── gif_upload.html     # อัปโหลด GIF
├── token.html          # จัดการ API token
├── logs.html           # ดู logs
├── header.html / footer.html
├── css/
│   ├── pico.min.css    # Pico CSS framework
│   └── style.css       # Custom styles
└── js/
    ├── main.js         # Bootstrap และ shared logic
    ├── utils.js        # Helper functions (fetchWithAuth, showToast, ฯลฯ)
    ├── alpinejs.min.js # Alpine.js สำหรับ reactive UI
    └── [feature]Handler.js   # Handler แยกตาม feature
```

### สร้างหน้า Web ใหม่

**1. สร้างไฟล์ `data/web/mypage.html`:**

```html
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>My Feature</title>
    <link rel="stylesheet" href="css/pico.min.css">
    <link rel="stylesheet" href="css/style.css">
</head>
<body>
    <!-- รวม header ด้วย fetch จาก main.js -->
    <div id="header-placeholder"></div>

    <main class="container">
        <h2>My Feature</h2>
        <div x-data="myFeatureHandler()">
            <button @click="doSomething()">Do Something</button>
            <p x-text="status"></p>
        </div>
    </main>

    <div id="footer-placeholder"></div>
    <script src="js/alpinejs.min.js" defer></script>
    <script src="js/utils.js"></script>
    <script src="js/main.js"></script>
    <script src="js/myFeatureHandler.js"></script>
</body>
</html>
```

**2. สร้าง `data/web/js/myFeatureHandler.js`:**

```javascript
function myFeatureHandler() {
    return {
        status: '',

        async doSomething() {
            // fetchWithAuth จาก utils.js — ใส่ Bearer token อัตโนมัติ
            const res = await fetchWithAuth('/api/v1/myfeature/status');
            const data = await res.json();
            this.status = data.value;
        }
    };
}
```

**3. เพิ่ม link ใน `data/web/header.html`** (ถ้าต้องการแสดงใน nav):

```html
<li><a href="mypage.html">My Feature</a></li>
```

---

## วิธี Build Firmware

### วิธีที่ 1 — PlatformIO CLI (แนะนำ)

```bash
# Build firmware เท่านั้น
pio run

# Build filesystem (LittleFS) เท่านั้น
pio run --target buildfs

# Build ทั้งคู่
pio run && pio run --target buildfs
```

ไฟล์ output อยู่ที่:

```
.pio/build/esp12e/firmware.bin
.pio/build/esp12e/littlefs.bin
```

### วิธีที่ 2 — Docker (ไม่ต้อง install PlatformIO)

```bash
./scripts/build-with-docker.sh
```

Script นี้ดึง Docker image `ghcr.io/times-z/devcontainer:latest` และ run ทั้ง `pio run` และ `pio run --target buildfs` ให้ครบ

### วิธีที่ 3 — VS Code Dev Container

เปิดโฟลเดอร์โปรเจกต์ใน VS Code แล้วเลือก **"Reopen in Container"**  
ภายใน container มี alias พร้อมใช้:

```bash
build      # = pio run
buildfs    # = pio run --target buildfs
```

---

## วิธี Build LittleFS

LittleFS คือ Filesystem ที่เก็บ:
- `config.json` — configuration
- `web/` — หน้า Web UI ทั้งหมด
- `gif/` — GIF files ที่อัปโหลด

**สิ่งที่จะถูกรวมอยู่ใน `littlefs.bin`** คือทุกไฟล์ภายใน `data/`

```bash
# Build LittleFS image
pio run --target buildfs
```

ไฟล์ที่ได้: `.pio/build/esp12e/littlefs.bin`

**Flash LittleFS ผ่าน OTA:**

```bash
bash scripts/flash-ota.sh <device-ip> --fs-only
```

หรือผ่าน Web UI ที่ `http://<device-ip>/update` (เลือก Filesystem)

> **หมายเหตุ:** LittleFS มี total size 2MB (จาก flash 4MB, แบ่ง 2MB ให้ firmware และ 2MB ให้ filesystem ตาม `eagle.flash.4m2m.ld`)

---

## Library ที่ใช้ (อ้างอิงใน platformio.ini)

| Library | Version | ใช้ทำอะไร |
|---------|---------|-----------|
| `bblanchon/ArduinoJson` | ^7.4.2 | Serialize/Deserialize JSON |
| `moononournation/GFX Library for Arduino` | ^1.6.4 | วาดรูปบนจอ ST7789 |
| `bitbank2/AnimatedGIF` | ^2.2.0 | Decode และเล่น GIF |

ติดตั้ง library เพิ่มเติมโดยเพิ่มใน `platformio.ini`:

```ini
lib_deps =
    bblanchon/ArduinoJson@^7.4.2
    moononournation/GFX Library for Arduino@^1.6.4
    bitbank2/AnimatedGIF@^2.2.0
    your-new-library@^1.0.0    ; เพิ่มที่นี่
```

---

## เอกสารสำหรับอ่านต่อ

### PlatformIO & ESP8266

| หัวข้อ | URL |
|--------|-----|
| PlatformIO Docs | [docs.platformio.org](https://docs.platformio.org/en/latest/) |
| ESP8266 Arduino Core | [arduino-esp8266.readthedocs.io](https://arduino-esp8266.readthedocs.io/en/latest/) |
| ESP8266 LittleFS | [arduino-esp8266.readthedocs.io/en/latest/filesystem.html](https://arduino-esp8266.readthedocs.io/en/latest/filesystem.html) |
| ESP8266HTTPUpdateServer | [github.com/esp8266/Arduino](https://github.com/esp8266/Arduino/tree/master/libraries/ESP8266HTTPUpdateServer) |

### Libraries

| Library | Docs |
|---------|------|
| ArduinoJson | [arduinojson.org/v7](https://arduinojson.org/v7/doc/) |
| Arduino GFX Library | [github.com/moononournation/Arduino_GFX](https://github.com/moononournation/Arduino_GFX) |
| AnimatedGIF | [github.com/bitbank2/AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) |

### Web UI

| หัวข้อ | URL |
|--------|-----|
| Pico.css | [picocss.com/docs](https://picocss.com/docs) |
| Alpine.js | [alpinejs.dev](https://alpinejs.dev) |

### API Reference

ดู OpenAPI spec ได้ที่ `swagger.yml` ในโปรเจกต์ หรือ import เข้า [Swagger Editor](https://editor.swagger.io) เพื่อดู UI

---

## Boot Flow สรุป

```
Power On
  └── setup()
        ├── LittleFS.begin()          # mount filesystem
        ├── SecureStorage.begin()      # init EEPROM secure storage
        ├── configManager.load()       # โหลด config.json
        ├── RescueMode.checkBootLoop() # ถ้า crash > N ครั้ง → Rescue Mode
        ├── DisplayManager.begin()     # init จอ ST7789 ผ่าน SPI
        ├── WiFiManager.begin()        # เชื่อมต่อ WiFi หรือเปิด AP
        ├── NTPClient.begin()          # เริ่ม NTP sync
        ├── Webserver.begin()          # เริ่ม HTTP server port 80
        └── registerApiEndpoints()    # ลง route ทั้งหมด

loop() (วนซ้ำตลอด)
  ├── webserver.handleClient()     # จัดการ HTTP request
  ├── ntpClient.loop()             # NTP periodic sync
  ├── DisplayManager.update()      # update GIF frame
  └── EspClass.wdtFeed()           # kick watchdog (ทุก 2 วินาที)
```
