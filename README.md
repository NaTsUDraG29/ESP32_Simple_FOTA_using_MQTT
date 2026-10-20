# ESP32 Smart IoT Dashboard & Weather Station 🛰️

A production-grade, dual-core embedded IoT system built on the **ESP32-WROVER-E** running **ESP-IDF v6.x** and **LVGL v8.4**. Features an ST7796S 480×320 SPI LCD with DMA, GT911 capacitive touch, TCA9534 I/O expansion, Wi-Fi station management, SNTP atomic clock sync, live Open-Meteo REST API weather telemetry, intelligent low-power inactivity display sleep, real-time MQTT cloud telemetry, and a **dual-partition Over-The-Air (OTA) firmware update engine with hardware bootloader rollback protection**.

---

## 🌟 System Highlights

* **Dual-Core FreeRTOS Architecture**:
  * **Core 0 (Networking & Background)**: Wi-Fi driver, LwIP TCP/IP stack, SNTP synchronization, asynchronous HTTPS REST client, `esp-mqtt` socket engine, and OTA streaming task.
  * **Core 1 (Presentation & User Input)**: Dedicated 60 FPS LVGL graphic engine with recursive mutex synchronization.
* **Production Dual-Partition OTA with Hardware Rollback Protection**:
  * **Dual Ping-Pong Partition Table** (`ota_0` and `ota_1`, 1920 KB each) with `otadata` (8 KB) on 4MB Flash.
  * **Bootloader Rollback Protection** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`): Newly flashed images boot in `ESP_OTA_IMG_PENDING_VERIFY` state. If the new app panics, hits a watchdog timeout, or crashes before validating itself, the hardware bootloader automatically reverts to the previous working slot on the next reboot.
  * **Self-Diagnostic Validation Gate**: Subsystem health checks (SPI, I2C, LCD, Touch, Netif) confirm proper startup before invoking `esp_ota_mark_app_valid_cancel_rollback()`.
  * **Live On-Screen Progress Modal**: Overlay on `lv_layer_top()` featuring an animated Cyan progress bar (0–100%), real-time byte counters (`Flashing: 45% (650 / 1443 KB)`), target URL, and status messages. Inactivity display sleep is suppressed during download.
  * **Remote MQTT Trigger**: Initiated remotely by publishing `{"cmd": "ota", "url": "..."}` to the device command topic. Supports HTTPS (Mozilla CA bundle) and plain HTTP for local development servers.
* **Wi-Fi Manager with Resilient Auto-Reconnect**:
  * Scans all 2.4 GHz channels and dynamically populates interactive list widgets.
  * On-screen touch QWERTY keyboard with masked password entry and toggleable visibility.
  * **5-attempt auto-reconnect state machine** with exponential/pacing backoff when router signal drops.
  * **Flash NVS Write-Wear Prevention**: "Read-Before-Write" verification gate eliminates redundant Flash sector erases.
  * **Multi-Subscriber Observer Pattern**: Broadcasts Wi-Fi state transitions to UI, SNTP, MQTT, and Weather services without tight coupling.
* **SNTP Real-Time Atomic Clock**:
  * 4-timestamp latency compensation algorithm synchronizing with `pool.ntp.org`.
  * POSIX timezone management for Indian Standard Time (`IST-5:30`, UTC+5:30) with automatic calendar and leap year tracking.
* **Live Cloud Weather Telemetry (HTTPS + cJSON)**:
  * Queries the **Open-Meteo REST API** over TLS (Port 443) using `esp_http_client`.
  * Verifies server certificates via Mozilla Root CA bundle (`esp_crt_bundle_attach`).
  * Gated on SNTP clock sync to prevent x.509 certificate validation errors.
  * Decodes WMO weather codes into real-time condition strings, temperature (°C), humidity (%), and wind speed (km/h).
* **Power-Saving Display Sleep & Tap-to-Wake**:
  * Automatically sleeps display after **30 seconds** of inactivity.
  * Cuts ~180 mA backlight current via TCA9534 P5 and commands ST7796S into Sleep In (`0x10`), dropping system power from ~275 mA to ~80 mA (>70% power savings).
  * Instant tap-to-wake via GT911 capacitive touch with **touch event suppression on wake** (prevents accidental button presses).
* **Real-Time MQTT Cloud Telemetry & Remote Control**:
  * Dual-core isolated: `esp-mqtt` socket engine runs on Core 0 with zero impact on LVGL 60 FPS graphics.
  * Publishes system metrics every 15s (uptime, free heap, RSSI, weather data, screen state) to `esp32_iot/<MAC>/telemetry`.
  * Supports bidirectional commands: remote sleep/wake, floating toast notifications, dynamic location weather refresh, and remote OTA updates.
  * Last Will and Testament (LWT) for automatic cloud presence detection on `broker.emqx.io`.
* **Thread-Safe Graphics Pipeline**:
  * Recursive mutex (`lvgl_port_lock()` / `lvgl_port_unlock()`) wrapping `lv_timer_handler()`.
  * Cross-thread decoupling flags bridge Core 0 networking events safely into Core 1 rendering loop.

---

## 🖥️ Screen Architecture (480 × 320 LCD)

```
┌────────────────────────────────────────────────────────┐
│  ● ONLINE       ● MQTT: CLOUD CONNECTED  [X Disconnect]│  <- Header Banner (H: 44px)
├────────────────────────────────────────────────────────┤
│                                                        │
│                      14:35:02                          │  <- Clock Card (H: 160px)
│                Sunday, 18 Oct 2026                     │     Montserrat 48 (Cyan)
│          ✓ SNTP Synced (pool.ntp.org)                  │
│                                                        │
├───────────────────────────┬────────────────────────────┤
│ Network Telemetry (Left)  │ Live Cloud Weather (Right) │  <- Dual-Card Bottom Bar
│ SSID: Home_5G             │ City: Bengaluru   24.8 °C  │     (H: 96px)
│ IP:   192.168.1.105       │ Partly Cloudy              │
│ RSSI: -54 dBm             │ Hum: 68%   Wind: 8.5 km/h  │
└───────────────────────────┴────────────────────────────┘
```

### Over-The-Air (OTA) Progress Modal (Overlay on `lv_layer_top`)

```
┌────────────────────────────────────────────────────────┐
│                                                        │
│      ┌──────────────────────────────────────────┐      │
│      │        ⬇  FIRMWARE UPGRADE (OTA)         │      │
│      │  http://192.168.1.50:8000/firmware.bin   │      │
│      │                                          │      │
│      │  [████████████████████░░░░░░░░░░░░░░░░]  │      │
│      │                   52%                    │      │
│      │      Flashing: 52% (750 / 1443 KB)       │      │
│      └──────────────────────────────────────────┘      │
│                                                        │
└────────────────────────────────────────────────────────┘
```

---

## 🔌 Hardware Pinout & Peripherals

| Peripheral | Controller | Bus / Interface | ESP32 GPIO | Description / Notes |
| :--- | :--- | :--- | :--- | :--- |
| **LCD MOSI** | ST7796S | SPI2 (FSPI) | `GPIO 4` | Master Out Slave In |
| **LCD CLK** | ST7796S | SPI2 (FSPI) | `GPIO 5` | SPI Clock (25 MHz with DMA) |
| **LCD DC** | ST7796S | GPIO | `GPIO 2` | Data / Command select line |
| **I2C SDA** | TCA9534 / GT911 | I2C0 | `GPIO 26` | 400 kHz Fast-Mode Data |
| **I2C SCL** | TCA9534 / GT911 | I2C0 | `GPIO 27` | 400 kHz Fast-Mode Clock |
| **Touch INT** | GT911 | GPIO | `GPIO 15` | Capacitive Touch Interrupt |
| **TCA9534 P0** | LCD Reset | I2C Expander (`0x27`) | — | Active Low LCD Panel Reset |
| **TCA9534 P1** | Touch Reset | I2C Expander (`0x27`) | — | Active Low GT911 CTP Reset |
| **TCA9534 P5** | LCD Backlight | I2C Expander (`0x27`) | — | Controls Q2 NPN Backlight Transistor |

---

## 💾 Flash Memory & Partition Table

Configured via [`partitions.csv`](partitions.csv) for standard 4MB Flash (`0x400000` bytes):

| Partition | Type | SubType | Flash Offset | Size | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **nvs** | `data` | `nvs` | `0x009000` | 24 KB (`0x6000`) | Wi-Fi credentials, non-volatile parameters |
| **otadata** | `data` | `ota` | `0x00F000` | 8 KB (`0x2000`) | Active boot slot selector & rollback state |
| **phy_init** | `data` | `phy` | `0x011000` | 4 KB (`0x1000`) | RF calibration data |
| **ota_0** | `app` | `ota_0` | `0x020000` | 1920 KB (`0x1E0000`) | Ping-pong application slot 0 (~1.875 MB) |
| **ota_1** | `app` | `ota_1` | `0x200000` | 1920 KB (`0x1E0000`) | Ping-pong application slot 1 (~1.875 MB) |

> **Firmware Headroom**: The built binary occupies `0x170320` bytes (~1.43 MB), providing **447 KB (23%) of free headroom** in each OTA partition.

---

## 📂 Project Structure

```
ESP32_WIFI_IOT/
├── CMakeLists.txt                              # Top-level project CMake configuration
├── partitions.csv                              # 4MB dual OTA ping-pong partition table
├── dependencies.lock                           # Locked IDF component versions
├── sdkconfig.defaults                          # Tracked Kconfig settings (4MB, OTA, LVGL fonts)
├── .gitignore                                  # Git exclusion rules
├── README.md                                   # Comprehensive project documentation
│
├── docs/                                       # Deep-dive architectural guides
│   ├── EMBEDDED_DEBUGGING_AND_POWER_GUIDE.md   # Register crash forensics & power modes
│   ├── TASK3_WEATHER_REST_API_ARCHITECTURE.md  # Open-Meteo HTTPS REST specification
│   ├── ESP_EVENT_AND_WIFI_TO_IP_DEEP_DIVE.md   # Wi-Fi to IP, MQTT & function pointer deep-dive
│   └── WIFI_TCP_IP_SNTP_ARCHITECTURE.md        # Network & SNTP system design
│
└── main/                                       # Firmware source files
    ├── CMakeLists.txt                          # Component registration
    ├── idf_component.yml                       # Managed dependencies (LVGL, cJSON)
    ├── main.c                                  # Entry point & FreeRTOS task spawns
    ├── system_init.c / .h                      # SPI and I2C hardware bus setup
    ├── tca9534.c / .h                          # I2C I/O expander driver (backlight & resets)
    ├── lcd.c / .h                              # ST7796S SPI driver & LVGL flush callback
    ├── gt911_touch.c / .h                      # GT911 touch driver & LVGL input callback
    ├── wifi_manager.c / .h                     # Station mode, NVS, retry machine, SNTP, Observer
    ├── wifi_ui.c / .h                          # LVGL screen builders, toast banner, OTA modal
    ├── weather_service.c / .h                  # HTTPS client & cJSON Open-Meteo parser
    ├── mqtt_service.c / .h                     # MQTT 3.1.1 telemetry & remote command engine
    └── ota_service.c / .h                      # Dual-partition OTA update & rollback manager
```

---

## 🛠️ Build and Flash

### Prerequisites
* **ESP-IDF**: `v6.x` (or `v5.1+`) installed and exported to your environment.
* **CMake & Ninja**: Installed via ESP-IDF tools installer.

### 1. Build the Firmware
```bash
idf.py set-target esp32
idf.py build
```
*(The IDF Component Manager will automatically download `lvgl/lvgl`, `espressif/cjson`, and `espressif/mqtt` into `managed_components/` on first build).*

### 2. Initial Flash to Hardware (Writes New Partition Table)
Because this project uses custom dual ping-pong partitions (`partitions.csv`), the first flash must write the bootloader, partition table, initial OTA data, and application image:

```bash
# Using standard IDF command:
idf.py -p COMx flash monitor

# Or using esptool directly:
python -m esptool --chip esp32 -p COMx -b 921600 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0xf000 build/ota_data_initial.bin 0x20000 build/ESP32_WIFI_IOT.bin
```

---

## 🔄 Dual-Partition OTA & Rollback Protection (Task 4)

### 1. Architectural Workflow

```
[Remote MQTT Command]
        │
        ▼ {"cmd": "ota", "url": "http://<IP>:8000/firmware.bin"}
[mqtt_service.c] (Core 0)
        │
        ▼ ota_service_start(url)
[ota_service.c] (Core 0 Background Task, 8KB Stack)
        ├── Connects via esp_https_ota (HTTP or HTTPS with Mozilla CA)
        ├── Dispatches atomic progress updates to wifi_ui.c
        ├── Writes chunks to passive partition (ota_0 ⇄ ota_1)
        ▼
[wifi_ui.c] (Core 1 GUI Task)
        └── Renders OTA Modal on lv_layer_top with Cyan Bar & Byte Counters
        │
        ▼ (Download Complete & Checksum Valid)
[otadata partition updated]
        │
        ▼ esp_restart()
[2nd Stage Bootloader]
        ├── Reads otadata: boots new slot in PENDING_VERIFY state
        ▼
[Application Boot: ota_service_validate_running_app()]
        ├── Hardware & peripheral self-diagnostics pass
        ├── Calls esp_ota_mark_app_valid_cancel_rollback()
        ▼
[Firmware Confirmed Permanent] ➔ Celebratory On-Screen Toast Banner!
```

### 2. Safety & Rollback Guarantee
If the new firmware suffers a kernel panic, memory corruption, brownout, or watchdog reset before calling `esp_ota_mark_app_valid_cancel_rollback()`, the ESP-IDF 2nd-stage bootloader detects that the image failed verification and **automatically boots back into the previously working firmware slot** on the next reset.

### 3. How to Perform an Over-The-Air Update

#### Step A: Host Firmware Locally
In your development terminal, start an HTTP server directly from the `build/` directory:
```powershell
cd C:\C_notes\ESP32_Bringup\ESP32_WIFI_IOT\build
python -m http.server 8000
```
Check your laptop's local Wi-Fi IP address with `ipconfig` (e.g. `192.168.1.50`).

#### Step B: Trigger the OTA Update via MQTT
Using **MQTTX**, the **EMQX Web Client**, or CLI, publish to your device's command topic:
* **Topic**: `esp32_iot/<CLIENT_ID>/cmd` (or broadcast `esp32_iot/all/cmd`)
* **Payload**:
  ```json
  {
    "cmd": "ota",
    "url": "http://192.168.1.50:8000/ESP32_WIFI_IOT.bin"
  }
  ```

#### Step C: Watch the Live Visual Update
1. The ESP32 screen dims and displays the **FIRMWARE UPGRADE (OTA)** modal.
2. The animated Cyan progress bar updates from **0% to 100%** with real-time byte counters.
3. The board validates SHA-256 integrity and reboots within 3 seconds.
4. On reboot, the bootloader switches partitions, verifies startup, and displays:
   > *"OTA Update Successful! Firmware validated."*

---

## 🛰️ Real-Time MQTT Cloud Telemetry & Remote Control

The device automatically connects to the public MQTT broker `broker.emqx.io` on port 1883 with Last Will and Testament (LWT) and auto-reconnect.

### 1. Topic Hierarchy
Each device derives a unique client ID and topic namespace from its hardware MAC address (`esp32_<MAC6>`):
- **Telemetry Stream**: `esp32_iot/<CLIENT_ID>/telemetry`
- **Device Commands**: `esp32_iot/<CLIENT_ID>/cmd`
- **Broadcast Commands**: `esp32_iot/all/cmd`
- **LWT Presence**: `esp32_iot/<CLIENT_ID>/status` (`"online"` / `"offline"`)
- **Command Response**: `esp32_iot/<CLIENT_ID>/response`

### 2. Telemetry Payload (Published every 15s)
```json
{
  "device": "ESP32-WROVER-E",
  "client_id": "esp32_246F28XXXXXX",
  "uptime_sec": 340,
  "free_heap": 178240,
  "min_free_heap": 152190,
  "display": "awake",
  "wifi": {
    "ssid": "MyWiFi",
    "ip": "192.168.1.105",
    "rssi": -55
  },
  "weather": {
    "city": "Bengaluru",
    "temp_c": 26.5,
    "condition": "Partly Cloudy",
    "humidity": 65,
    "wind_kmh": 12.4
  }
}
```

### 3. Remote Control Commands
Publish any of the following JSON payloads to `esp32_iot/<CLIENT_ID>/cmd` or `esp32_iot/all/cmd`:

* **Over-The-Air Firmware Upgrade**:
  ```json
  {"cmd": "ota", "url": "http://192.168.1.50:8000/ESP32_WIFI_IOT.bin"}
  ```
* **Display Power Control**:
  ```json
  {"cmd": "display", "action": "sleep"}
  {"cmd": "display", "action": "wake"}
  ```
* **Instant On-Screen Notification Toast**:
  ```json
  {"cmd": "toast", "msg": "Doorbell Rang at Front Gate!", "duration_ms": 5000}
  ```
* **Dynamic Location Weather Change**:
  ```json
  {"cmd": "set_city", "name": "London", "lat": 51.5074, "lon": -0.1278}
  ```
* **Ping / Telemetry On-Demand**:
  ```json
  {"cmd": "ping"}
  ```

---

## 🗺️ Roadmap
- [x] **Task 1**: Standalone Hardware Bringup (ST7796S SPI, GT911 I2C, TCA9534, LVGL v8.4).
- [x] **Task 2**: Wi-Fi Station Manager, NVS Persistence, SNTP Sync, Display Sleep & Tap-to-Wake.
- [x] **Task 3A**: Live Cloud Weather REST API Client (HTTPS Open-Meteo + cJSON).
- [x] **Task 3B**: MQTT 3.1.1/5.0 Cloud Telemetry & Remote Control (`esp-mqtt`).
- [x] **Task 4**: Production Dual-Partition OTA Updates with Bootloader Rollback & Live UI Progress Modal.

---

## 📄 License
This project is licensed under the MIT License.
