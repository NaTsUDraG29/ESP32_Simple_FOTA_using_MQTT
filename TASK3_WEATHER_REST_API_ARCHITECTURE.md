# Task 3A: Live Cloud Weather & REST API Client (HTTPS + cJSON)

This document describes the design, network protocol flow, security considerations, and GUI architecture for the ESP32 Live Cloud Weather Service.

---

## 1. System Overview

Task 3A connects the ESP32 IoT Dashboard to the global Internet using **HTTPS (HyperText Transfer Protocol Secure)** and **cJSON** to retrieve and display live weather telemetry from the **Open-Meteo REST API**.

```
                           INTERNET / CLOUD
                                  │
                                  ▼ (HTTPS / TLS 1.3 Port 443)
                    ┌───────────────────────────┐
                    │     api.open-meteo.com    │
                    └─────────────┬─────────────┘
                                  │
               ┌──────────────────┴──────────────────┐
               │         ESP32 Dual-Core SoC         │
               │                                     │
               │  [CORE 0: Networking & Background]  │
               │  ├── Wi-Fi Station & Netif Stack    │
               │  ├── SNTP Time Service              │
               │  └── weather_task (Priority 4)      │
               │      ├── esp_http_client (HTTPS)    │
               │      ├── esp_crt_bundle (TLS x509)  │
               │      └── cJSON Parser               │
               │                  │                  │
               │                  │ Mutex Handoff    │
               │                  ▼                  │
               │  [CORE 1: Graphical Presentation]   │
               │  └── gui_task (Priority 5)          │
               │      ├── lv_timer_handler()         │
               │      └── Dual-Card Bottom Bar:      │
               │          ├── Network Telemetry Card │
               │          └── Live Weather Card      │
               └─────────────────────────────────────┘
```

---

## 2. The REST API & Payload Specification

* **Target Endpoint**: `https://api.open-meteo.com/v1/forecast`
* **Query Parameters**:
  * `latitude=12.9716`
  * `longitude=77.5946` (Default: Bengaluru, India)
  * `current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m`
* **Transport**: HTTPS over TCP port 443 with TLS encryption.
* **Why Open-Meteo**: 100% free for embedded non-commercial devices, no API keys, zero rate-limit hassles.

### Sample Incoming JSON Payload (~350 bytes):
```json
{
  "latitude": 12.971599,
  "longitude": 77.59456,
  "elevation": 917.0,
  "current_units": {
    "temperature_2m": "°C",
    "relative_humidity_2m": "%",
    "weather_code": "wmo code",
    "wind_speed_10m": "km/h"
  },
  "current": {
    "time": "2026-10-18T01:30",
    "temperature_2m": 24.8,
    "relative_humidity_2m": 72,
    "weather_code": 2,
    "wind_speed_10m": 9.2
  }
}
```

---

## 3. Critical Embedded TLS Gotcha: Clock Dependency

When connecting to an HTTPS server, the ESP32 must validate the server's **x.509 SSL certificate** via `esp_crt_bundle_attach`.

Every SSL certificate has a validity time window:
* `Not Before`: e.g., `Jan 10, 2024`
* `Not After`: e.g., `Jan 10, 2027`

> [!IMPORTANT]
> If the ESP32 boots and attempts an HTTPS request before SNTP synchronizes, its internal clock is sitting at **Unix Epoch 0 (`Jan 1, 1970`)**.
> The TLS handshake will immediately fail with error:
> `MBEDTLS_ERR_X509_CERT_VERIFY_FAILED: Certificate is not yet valid`
> 
> Therefore, [`weather_service.c`](file:///c:/C_notes/ESP32_Bringup/ESP32_WIFI_IOT/main/weather_service.c) explicitly gates its execution on both:
> `wifi_manager_get_state() == WIFI_STATE_CONNECTED && wifi_manager_is_time_synced()`.

---

## 4. WMO Weather Code Decoding

The Open-Meteo API returns standard World Meteorological Organization (WMO) integer codes. `weather_service.c` maps these to clean UI strings:

| WMO Code | Meaning | Displayed Condition |
| :--- | :--- | :--- |
| `0` | Clear sky | `Clear Sky` |
| `1, 2, 3` | Mainly clear, partly cloudy, overcast | `Mainly Clear`, `Partly Cloudy`, `Overcast` |
| `45, 48` | Fog and depositing rime fog | `Foggy` |
| `51, 53, 55` | Drizzle (Light / Moderate / Heavy) | `Drizzle` |
| `61, 63, 65` | Rain (Slight / Moderate / Heavy) | `Light Rain`, `Moderate Rain`, `Heavy Rain` |
| `71, 73, 75` | Snow fall | `Snow` |
| `80, 81, 82` | Rain showers | `Rain Showers` |
| `95, 96, 99` | Thunderstorm (with/without hail) | `Thunderstorm` |

---

## 5. UI Layout (480 × 320 LCD)

The bottom area of the Connected Clock Dashboard (Y: 218 to 314 px) is divided into two balanced, modern cards:

```
┌────────────────────────────────────────────────────────┐
│  ● ONLINE                             [X Disconnect]   │  <- Top Banner (H: 44px)
├────────────────────────────────────────────────────────┤
│                                                        │
│                      14:35:02                          │  <- Clock Card (H: 160px)
│                Sunday, 18 Oct 2026                     │     Montserrat 48 (Cyan)
│          ✓ SNTP Synced (pool.ntp.org)                  │
│                                                        │
├───────────────────────────┬────────────────────────────┤
│ Network Info (W: 224px)   │ Live Weather (W: 226px)    │  <- Balanced Bottom Bar
│ SSID: Home_5G             │ 📍 Bengaluru      24.8°C   │     (H: 96px)
│ IP:   192.168.1.105       │ Partly Cloudy              │
│ RSSI: -54 dBm             │ 💧 72%    💨 9 km/h        │
└───────────────────────────┴────────────────────────────┘
```

1. **Network Card (Left - 224 × 96 px)**:
   * SSID (White)
   * IP address (Cyan)
   * Signal strength RSSI (Slate Gray)
2. **Weather Card (Right - 226 × 96 px)**:
   * City Name (`📍 Bengaluru`)
   * Temperature (`24.8°C`) in bold, prominent Montserrat 24 Amber font
   * Weather Condition (`Partly Cloudy` in Cyan)
   * Humidity & Wind Speed (`💧 72%   💨 9 km/h`)
