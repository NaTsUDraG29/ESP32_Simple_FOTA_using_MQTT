# ESP32 Smart IoT Dashboard & Weather Station 🛰️

A production-grade, dual-core embedded IoT system built on the **ESP32-WROVER-E** running **ESP-IDF v6.x** and **LVGL v8.4**. Features an ST7796S 480×320 SPI LCD with DMA, GT911 capacitive touch, TCA9534 I/O expansion, Wi-Fi station management, SNTP atomic clock sync, live Open-Meteo REST API weather telemetry, and intelligent low-power inactivity display sleep.

---

## 🌟 System Highlights

* **Dual-Core FreeRTOS Architecture**:
  * **Core 0 (Networking & Background)**: Wi-Fi driver, LwIP TCP/IP stack, SNTP synchronization, and asynchronous HTTPS REST client.
  * **Core 1 (Presentation & User Input)**: Dedicated 60 FPS LVGL graphic engine with recursive mutex synchronization.
* **Wi-Fi Manager with Resilient Auto-Reconnect**:
  * Scans all 2.4 GHz channels and dynamically populates interactive list widgets.
  * On-screen touch QWERTY keyboard with masked password entry and toggleable visibility.
  * **5-attempt auto-reconnect state machine** with exponential/pacing backoff when router signal drops.
  * **Flash NVS Write-Wear Prevention**: "Read-Before-Write" verification gate eliminates redundant Flash sector erases.
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
  * Supports bidirectional commands: remote sleep/wake, floating toast notifications, dynamic location weather refresh, and OTA update hooks.
  * Last Will and Testament (LWT) for automatic cloud presence detection.
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

## 📂 Project Structure

```
ESP32_WIFI_IOT/
├── CMakeLists.txt                              # Top-level project CMake file
├── dependencies.lock                           # Locked IDF component versions
├── sdkconfig.defaults                          # Tracked Kconfig settings (fonts, heap, float)
├── .gitignore                                  # Git exclusion rules
├── README.md                                   # Project documentation
│
├── docs/                                       # Deep-dive architectural guides
│   ├── EMBEDDED_DEBUGGING_AND_POWER_GUIDE.md   # Register crash forensics & power modes
│   ├── TASK3_WEATHER_REST_API_ARCHITECTURE.md  # Open-Meteo HTTPS REST specification
│   ├── ESP_EVENT_AND_WIFI_TO_IP_DEEP_DIVE.md   # Wi-Fi to IP state traversal analysis
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
    ├── wifi_manager.c / .h                     # Station mode, NVS, retry machine, SNTP
    ├── wifi_ui.c / .h                          # LVGL screen builders & timer rendering
    └── weather_service.c / .h                  # HTTPS client & cJSON Open-Meteo parser
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
*(The IDF Component Manager will automatically download `lvgl/lvgl` and `espressif/cjson` into `managed_components/` on first build).*

### 2. Flash to Hardware
```bash
# Replace COMx with your board's serial port (e.g. COM7 on Windows, /dev/ttyUSB0 on Linux)
idf.py -p COMx flash monitor
```

---

## 🚀 Execution Flow

1. **Boot**: Initializes SPI, I2C, and sets up TCA9534 expander to power on display and touch.
2. **UI Mount**: Creates setup screen and clock screen in RAM, starts 2ms hardware LVGL tick.
3. **Core Task Spawn**: Spawns `gui_task` on **Core 1** and `weather_task` on **Core 0**.
4. **Wi-Fi Check**:
   * If credentials exist in NVS -> Automatically connects.
   * If first boot -> Launches active 2.4 GHz channel scan and lists SSIDs.
5. **Connection**: User selects network, enters password on virtual keyboard, associations complete, DHCP assigns IP.
6. **Time & Cloud Sync**: SNTP synchronizes with `pool.ntp.org`, system time is set to IST, and `weather_task` queries `api.open-meteo.com` over HTTPS.
7. **Display**: Shows large digital clock, date, network telemetry, and live weather telemetry.
8. **Inactivity**: After 30 seconds of no touch input, screen enters low-power sleep; tapping anywhere instantly wakes the panel.

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
* **OTA Firmware Update Trigger (Hook for Task 4)**:
  ```json
  {"cmd": "ota", "url": "http://192.168.1.50:8000/firmware.bin"}
  ```

### 4. How to Test via EMQX Online Web Client
1. Open the free public EMQX Web Client in any browser: [EMQX Online MQTT Client](http://www.emqx.io/online-mqtt-client/) (or [MQTTX Web](https://mqttx.app/web/)).
2. Click **New Connection** / **Connect** (default host `broker.emqx.io`, port `8083` or `8084` WSS).
3. In **Subscriptions**, click **New Subscription**:
   - Topic: `esp32_iot/#` (to monitor all telemetry packets and command acknowledgments).
4. In **Publish**, enter:
   - Topic: `esp32_iot/all/cmd`
   - Message: `{"cmd": "toast", "msg": "Hello from EMQX Cloud!"}`
5. Watch the ESP32 LCD immediately display the pill toast banner!

---

## 🗺️ Roadmap
- [x] **Task 1**: Standalone Hardware Bringup (ST7796S SPI, GT911 I2C, TCA9534, LVGL).
- [x] **Task 2**: Wi-Fi Station Manager, NVS Persistence, SNTP Sync, Display Sleep.
- [x] **Task 3A**: Live Cloud Weather REST API Client (HTTPS + cJSON).
- [x] **Task 3B**: MQTT 3.1.1/5.0 Cloud Telemetry & Remote Control (`esp-mqtt`).
- [ ] **Task 4**: Dual-Partition OTA (Over-The-Air) Firmware Updates.

---

## 📄 License
This project is licensed under the MIT License.

