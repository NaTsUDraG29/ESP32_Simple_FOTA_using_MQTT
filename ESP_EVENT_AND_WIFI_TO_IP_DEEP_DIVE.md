# ESP-IDF Event Architecture & The Wi-Fi to IP Journey: Deep Dive

---

## 1. The Core Mystery: Why Does It Switch From `WIFI_EVENT` to `IP_EVENT`?

To understand why ESP-IDF switches from `WIFI_EVENT` to `IP_EVENT`, you must understand the **OSI Network Layer Separation**:

```
+-----------------------------------------------------------------------------+
| LAYER 3 (Network Layer) : IP Addresses, Routing, Subnets, DHCP              |
| Handled by: LwIP Stack (TCP/IP Task)                                        |
| Emits:      IP_EVENT (e.g., IP_EVENT_STA_GOT_IP)                            |
+-----------------------------------------------------------------------------+
                                      ▲
                                      │ Hand-off (Triggered by esp_netif)
                                      ▼
+-----------------------------------------------------------------------------+
| LAYER 2 (Data Link / MAC): Radio Channels, Antennas, Beacons, WPA2 Handshake|
| Handled by: Wi-Fi Driver Task ("wifi") & RF Hardware                        |
| Emits:      WIFI_EVENT (e.g., WIFI_EVENT_STA_CONNECTED)                     |
+-----------------------------------------------------------------------------+
```

### The Boundary Line
* The **Wi-Fi Hardware & Driver** (`esp_wifi`) only understand **radio waves, MAC addresses, and RF encryption**. The Wi-Fi driver has **ZERO knowledge** of what an IP address is, what a router is, or what DHCP is! Its only job is to establish a physical radio link with the Access Point.
* The **TCP/IP Stack** (`LwIP`) understands **IP addresses, routing tables, and internet protocols**. But it has **ZERO knowledge** of antennas, radio signal strength, or Wi-Fi passwords!
* **The Bridge**: The `esp_netif` component (which you initialized with `esp_netif_create_default_wifi_sta()`) listens for the radio link to be established. The moment the radio says *"I am connected to the antenna"*, `esp_netif` tells the TCP/IP stack: *"Start the DHCP negotiation over this link!"*
* Once DHCP succeeds in getting an IP address from your home router, the **TCP/IP stack** emits `IP_EVENT_STA_GOT_IP`.

---

## 2. The Internal Data Structures: The Hierarchical Linked-List Tree

In your code in `wifi_manager.c`, you registered two event handlers:
```c
// Registration 1: Listen to ALL Wi-Fi events
esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);

// Registration 2: Listen ONLY to the Got IP event
esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
```

### How Does the ESP32 Store This in RAM?
ESP-IDF does not use a flat array (like the hardware NVIC table). It allocates a **3-tier hierarchical singly-linked list (SLIST)** on the heap:

```
[ s_default_loop (esp_event_loop_instance_t) ]
  │
  ├── queue: FreeRTOS QueueHandle (holds incoming event messages)
  ├── task:  FreeRTOS TaskHandle  ("sys_evt" worker task)
  ├── mutex: FreeRTOS Semaphore   (guards list against concurrent writes)
  │
  └── loop_nodes (Linked List of Loop Level Containers)
        │
        └── esp_event_loop_node_t
              │
              ├── handlers: (Handlers registered for literally ALL events in the system)
              │
              └── base_nodes: Singly-Linked List of Event Bases
                    │
                    ├── [Node 1: Base = "WIFI_EVENT"]
                    │     │
                    │     ├── handlers: (Handlers registered for ESP_EVENT_ANY_ID)
                    │     │     │
                    │     │     └── [Handler Node]
                    │     │           ├── handler: &wifi_event_handler
                    │     │           ├── arg:     NULL
                    │     │           └── next:    NULL
                    │     │
                    │     └── id_nodes: (Empty, because we registered for ANY ID!)
                    │
                    └── [Node 2: Base = "IP_EVENT"] ──► (points to next base node)
                          │
                          ├── handlers: (Empty, no ANY_ID registered here)
                          │
                          └── id_nodes: Singly-Linked List of Specific Event IDs
                                │
                                └── [ID Node: id = IP_EVENT_STA_GOT_IP (1)]
                                      │
                                      └── handlers:
                                            │
                                            └── [Handler Node]
                                                  ├── handler: &wifi_event_handler
                                                  ├── arg:     NULL
                                                  └── next:    NULL
```

---

## 3. Why Is It Designed Like This? (Architectural Rationale)

Why did Espressif's software architects design a 3-tier linked list instead of simpler structures?

### Reason 1: String Pointer Identity (`base_node->base == post.base`)
* Look at how `WIFI_EVENT` is defined in ESP-IDF:
  ```c
  ESP_EVENT_DECLARE_BASE(WIFI_EVENT);
  // Expands to: extern const char* WIFI_EVENT;
  // In C file:  const char* WIFI_EVENT = "WIFI_EVENT";
  ```
* Because `WIFI_EVENT` and `IP_EVENT` are unique pointer addresses to string literals in constant flash memory, **ESP-IDF never calls `strcmp()`!**
* It compares pointers using a single CPU instruction:
  ```c
  if (base_node->base == post.base) // Fast 1-cycle pointer comparison!
  ```

### Reason 2: Wildcard Matching (`ESP_EVENT_ANY_ID` & `ESP_EVENT_ANY_BASE`)
* In our Wi-Fi manager, we want our callback to receive `WIFI_EVENT_STA_START`, `WIFI_EVENT_SCAN_DONE`, `WIFI_EVENT_STA_CONNECTED`, and `WIFI_EVENT_STA_DISCONNECTED`.
* By putting the handler on the `base_node->handlers` list (the base level), a single registration automatically captures **every current and future ID** under that base without registering 20 individual handlers.

### Reason 3: 1-to-Many Multicast (Multiple Independent Listeners)
* In a hardware NVIC table, only **one** function can be the ISR for an interrupt.
* In a complex operating system, **multiple independent modules** need to know when an IP is acquired:
  1. Your Application (`wifi_manager.c`) wants to update the UI clock.
  2. The SNTP Client wants to start time synchronization.
  3. The mDNS Service wants to announce `esp32.local` on the network.
  4. An HTTP Web Server wants to start listening on port 80.
* Because each ID node contains a linked list of `handlers`, **all four modules can register their own callbacks** without overwriting each other. When `IP_EVENT_STA_GOT_IP` fires, the event loop walks the list and calls all four in sequence!

---

## 4. The Complete Execution Trace: From IDLE to GOT_IP

Here is the exact, step-by-step journey through every function, task, queue, and pointer in the system when you connect to Wi-Fi.

---

### Step 1: The User Initiates Connection
* In your code, you call:
  ```c
  wifi_manager_connect("Home_WiFi", "password123");
  ```
* This function calls:
  ```c
  esp_wifi_connect();
  ```
* The calling task returns immediately. The Wi-Fi radio takes over asynchronously in the background.

---

### Step 2: The Physical Radio Negotiation (Layer 2)
The dedicated FreeRTOS task **`wifi`** (running on Core 0 at high priority 23) talks to the RF hardware:
1. **Probe Request / Response**: Finds the channel of `"Home_WiFi"`.
2. **Authentication Frame**: Exchanges 802.11 Open System Authentication.
3. **Association Request / Response**: Router assigns Association ID (AID).
4. **4-Way WPA2 Handshake**:
   * ESP32 and Router exchange 4 EAPOL packets.
   * Cryptographic keys (PTK and GTK) are generated to encrypt the radio link.

---

### Step 3: Wi-Fi Driver Posts `WIFI_EVENT_STA_CONNECTED`
The 4-way handshake succeeds! The radio link is up.
The `wifi` task does **not** call your function directly. Instead, it posts an event:

```c
// Inside ESP-IDF Wi-Fi Driver:
esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &event_data, sizeof(event_data), timeout);
```

#### What happens inside `esp_event_post()`?
1. It creates an `esp_event_post_instance_t` struct on the heap containing:
   * `base`: Pointer to `WIFI_EVENT`
   * `id`: `WIFI_EVENT_STA_CONNECTED`
   * `data`: SSID, channel, and BSSID information
2. It pushes this struct into the **`sys_evt` queue** (`xQueueSend(loop->queue, ...)`).
3. The Wi-Fi task continues its radio maintenance.

---

### Step 4: The Event Loop Task (`sys_evt`) Dispatches `WIFI_EVENT`
The dedicated background task **`sys_evt`** was sleeping in `xQueueReceive()`. The arrival of the event wakes it up!

Now, look at the traversal algorithm executing inside `sys_evt` (from `esp_event.c`):

```
sys_evt Task:
  │
  1. Pop event from queue: {base = "WIFI_EVENT", id = 4 (WIFI_EVENT_STA_CONNECTED)}
  │
  2. Walk base_nodes linked list:
     ├── Check Node 1: base_node->base ("WIFI_EVENT") == post.base ("WIFI_EVENT")? 
     │   └── MATCH! (Pointers are identical)
     │
     ├── Step 2A: Check base_node->handlers (Handlers registered with ESP_EVENT_ANY_ID):
     │   └── Found Handler Node!
     │       └── Calls: (*handler)(&wifi_event_handler, NULL, "WIFI_EVENT", 4, &data)
     │           │
     │           ▼
     │       YOUR CODE EXECUTES:
     │       wifi_event_handler() receives WIFI_EVENT_STA_CONNECTED:
     │       Logs: "Associated with AP. Requesting IP address via DHCP..."
     │
     └── Step 2B: Check base_node->id_nodes (Specific ID handlers):
         └── None registered. Done with WIFI_EVENT!
```

---

### Step 5: The Handoff — How Does It Switch to DHCP?
**This is the crucial link:**
When you called `esp_netif_create_default_wifi_sta()` at startup, ESP-IDF registered an **internal system handler** behind your back!

```
                    WIFI_EVENT_STA_CONNECTED Occurs!
                                    │
               ┌────────────────────┴────────────────────┐
               ▼                                         ▼
   Your User Callback                       esp_netif Internal Callback
   (wifi_event_handler)                     (esp_netif_action_connected)
   - Prints log: "Associated..."             - Starts LwIP DHCP Client!
```

1. Alongside your handler, `esp_netif`'s internal callback `esp_netif_action_connected()` also executes.
2. It calls the LwIP function:
   ```c
   dhcp_start(esp_netif->lwip_netif);
   ```
3. The Wi-Fi layer has officially handed off control to the **TCP/IP layer**!

---

### Step 6: DHCP Negotiation Over the Radio (Layer 3)
Now, the **`tiT` (TCP/IP Task)** takes over:
1. LwIP builds a **`DHCP DISCOVER`** UDP packet (destined for `255.255.255.255:67`).
2. LwIP hands the packet to `esp_netif`, which hands it to the Wi-Fi driver, which transmits it over the antenna.
3. The home router replies with **`DHCP OFFER`** (`192.168.1.105`).
4. LwIP transmits **`DHCP REQUEST`**.
5. The home router replies with **`DHCP ACK`** containing:
   * Assigned IP: `192.168.1.105`
   * Subnet Mask: `255.255.255.0`
   * Default Gateway: `192.168.1.1`
   * DNS Server: `192.168.1.1`
6. LwIP commits these values to the virtual network interface `wlan0`.

---

### Step 7: LwIP Posts `IP_EVENT_STA_GOT_IP`
Now that the interface has an actual IP address, LwIP calls:

```c
// Inside LwIP / esp_netif:
ip_event_got_ip_t got_ip_data;
got_ip_data.ip_info.ip = ... (192.168.1.105);
esp_event_post(IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip_data, sizeof(got_ip_data), timeout);
```

Notice what changed:
* The `base` is now **`IP_EVENT`** (a completely different string pointer address!).
* The `id` is now **`IP_EVENT_STA_GOT_IP`** (`1`).

---

### Step 8: `sys_evt` Dispatches `IP_EVENT` to Your Handler

The packet enters the `sys_evt` queue. The task wakes up again and traverses the linked-list tree:

```
sys_evt Task:
  │
  1. Pop event from queue: {base = "IP_EVENT", id = 1 (IP_EVENT_STA_GOT_IP)}
  │
  2. Walk base_nodes linked list:
     │
     ├── Check Node 1: base_node->base ("WIFI_EVENT") == post.base ("IP_EVENT")?
     │   └── NO MATCH. Move to next base node.
     │
     └── Check Node 2: base_node->base ("IP_EVENT") == post.base ("IP_EVENT")?
         └── MATCH! (Pointers are identical)
             │
             ├── Step 8A: Check base_node->handlers:
             │   └── None (we registered with specific ID, not ANY_ID).
             │
             └── Step 8B: Check base_node->id_nodes linked list:
                 │
                 └── Walk id_nodes looking for id == 1 (IP_EVENT_STA_GOT_IP):
                     └── MATCH! Found ID Node for GOT_IP!
                         │
                         └── Walk id_node->handlers:
                             └── Found Handler Node!
                                 └── Calls: (*handler)(&wifi_event_handler, NULL, "IP_EVENT", 1, &got_ip_data)
                                     │
                                     ▼
                                 YOUR CODE EXECUTES:
                                 wifi_event_handler() receives IP_EVENT_STA_GOT_IP:
                                 1. Copies IP ("192.168.1.105"), Netmask, Gateway to s_ip_info.
                                 2. Sets WIFI_CONNECTED_BIT in FreeRTOS Event Group.
                                 3. Saves credentials in Flash NVS.
                                 4. Starts SNTP time sync!
```

---

## 5. Summary: The Complete Wi-Fi to IP System Design Picture

| Stage | Responsible Layer & Task | What Physical Action Occurs? | What Event Is Emitted? | Who Receives It? |
| :--- | :--- | :--- | :--- | :--- |
| **1. Connect** | App Task (`main`) | Calls `esp_wifi_connect()` | *None* | Wi-Fi Driver Task |
| **2. Radio Association** | Wi-Fi Driver Task (`wifi`, Core 0) | 802.11 Auth, Assoc, & 4-Way WPA2 Handshake | **`WIFI_EVENT_STA_CONNECTED`** | 1. Your `wifi_event_handler`<br/>2. `esp_netif` internal callback |
| **3. DHCP Negotiation** | TCP/IP Task (`tiT` / LwIP, Core 0) | Broadcasts DHCP Discover $\to$ Router replies with IP lease | *None (UDP packets on wire)* | Router DHCP Server |
| **4. IP Acquisition** | TCP/IP Task (`tiT` / LwIP, Core 0) | Commits IP `192.168.1.105` to network interface | **`IP_EVENT_STA_GOT_IP`** | Your `wifi_event_handler` |
| **5. Application Response** | Event Loop Task (`sys_evt`, Core 0) | Executes your callback: updates UI, saves NVS, triggers SNTP | *None* | `gui_task` (Core 1) & SNTP |

---

## 6. The Next Step in the Pipeline: From IP Acquisition to Cloud MQTT Telemetry

Once the TCP/IP stack successfully commits an IP lease and fires `IP_EVENT_STA_GOT_IP`, the device is reachable on the Local Area Network (LAN). But an IoT device must communicate with the world.

This brings us to **Layer 7 (Application Layer)**: **MQTT (Message Queuing Telemetry Transport)** over TCP Port 1883.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ LAYER 7 (Application Layer) : MQTT Protocol & JSON Telemetry                │
│ Handled by: esp-mqtt Client Task & mqtt_telemetry_task (Core 0)             │
│ Transports: Telemetry (Heap, RSSI, Weather), Remote Commands (Toast, Sleep) │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │ Raw TCP Stream (Port 1883)
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ LAYER 4 (Transport Layer)   : TCP Protocol                                  │
│ Handled by: LwIP Stack ("tiT" Task, Core 0)                                 │
│ Manages:    SYN, ACK, Sequence Numbers, Retransmissions, Keep-Alive PINGs   │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │ IP Packets & Routing
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ LAYER 3 (Network Layer)     : IPv4 Addressing & DHCP                        │
│ Status:     Acquired via IP_EVENT_STA_GOT_IP (e.g. 10.135.176.12)           │
└─────────────────────────────────────────────────────────────────────────────┘
```

### The MQTT Connection Sequence:
1. **Trigger**: When `wifi_manager.c` transitions to `WIFI_STATE_CONNECTED`, it notifies all registered observers.
2. **DNS Resolution**: `mqtt_service` takes the broker hostname (`broker.emqx.io`) and asks LwIP to resolve it to an IPv4 address via DNS (UDP Port 53).
3. **TCP 3-Way Handshake**: LwIP initiates a TCP connection on port 1883 (`SYN` $\to$ `SYN-ACK` $\to$ `ACK`).
4. **MQTT CONNECT Packet**: `esp-mqtt` sends a binary `CONNECT` frame containing:
   * **Client ID**: `esp32_<MAC6>` (e.g., `esp32_FCB46790F570` derived from hardware eFuse).
   * **Keep-Alive**: 60 seconds.
   * **Clean Session**: `1`.
   * **Last Will & Testament (LWT)**: Topic `esp32_iot/<CLIENT_ID>/status`, Payload `"offline"`, QoS 1, Retain `1`.
5. **MQTT CONNACK Packet**: The cloud broker (`broker.emqx.io`) responds with `0x20 0x02 0x00 0x00` (Connection Accepted).
6. **Auto-Subscriptions**: The ESP32 immediately subscribes to:
   * Device-specific commands: `esp32_iot/<CLIENT_ID>/cmd`
   * Broadcast commands: `esp32_iot/all/cmd`
7. **Birth Certificate**: It publishes `"online"` (retained) to `esp32_iot/<CLIENT_ID>/status`.

---

## 7. The Multi-Subscriber Observer Pattern & Function Pointer Mechanics

### The Single-Callback Overwrite Trap
During early development, `wifi_manager.c` had a single callback variable:
```c
static wifi_state_cb_t s_state_cb = NULL; // Only 1 slot!
```
When `wifi_ui_init()` registered its handler, `s_state_cb` pointed to the UI. But when `mqtt_service_init()` registered next, it **overwrote** `s_state_cb`. Consequently, when Wi-Fi connected, only MQTT was notified—the UI never received `WIFI_STATE_CONNECTED`, freezing the display on the setup screen!

### The Observer Pattern Solution
To decouple subsystems permanently, `wifi_manager.c` maintains an **Observer Array**:

```c
#define MAX_STATE_OBSERVERS 4

typedef struct {
    wifi_state_cb_t cb;         // Function pointer address in Flash
    void           *user_data;  // Context luggage tag
} wifi_state_observer_t;

static wifi_state_observer_t s_state_observers[MAX_STATE_OBSERVERS] = {0};
```

When `set_state(WIFI_STATE_CONNECTED)` is called:
```c
static void set_state(wifi_state_t new_state) {
    s_current_state = new_state;
    for (int i = 0; i < MAX_STATE_OBSERVERS; i++) {
        if (s_state_observers[i].cb) {
            s_state_observers[i].cb(new_state, s_state_observers[i].user_data);
        }
    }
}
```

### Deep Dive: Function Pointer Assembly & CPU Control Jump
What actually happens in silicon when `s_state_observers[i].cb(...)` executes?

```
   wifi_ui.c                                                 wifi_manager.c
 ═════════════                                             ══════════════════
 1. on_wifi_state_changed()                                2. Storage array in RAM:
    lives at address: 0x400D5820                              s_state_observers[0].cb = NULL
                                                              
 3. Hand over address:                                     4. Save address:
    register_cb(0x400D5820, NULL)  ──[Registration]───────>  s_state_observers[0].cb = 0x400D5820
                                                              
                                                                     ... (Time passes) ...
                                                              
                                                           5. Wi-Fi connects!
                                                              Calls: s_state_observers[0].cb(...)
 6. CPU Program Counter jumps                              
    to 0x400D5820!                 <──[CPU Jump]───────────
    s_pending_show_clock = true;
                                                              
 7. Function hits return;          ───[CPU Return]────────> 8. Continues next line in wifi_manager!
```

1. **Syntax Definition**:
   ```c
   typedef void (*wifi_state_cb_t)(wifi_state_t new_state, void *user_data);
   ```
   `wifi_state_cb_t` is a 4-byte pointer type holding the entry memory address of executable code matching this signature.
2. **Function Address**: In C, writing the function name without parentheses (`on_wifi_state_changed`) evaluates directly to its raw entry point address in Flash (e.g. `0x400D5820`).
3. **Register Loading (Xtensa ABI)**:
   * The CPU loads `0x400D5820` into address register `a8`.
   * Argument 1 (`new_state = 5`) is placed into register `a2`.
   * Argument 2 (`user_data = NULL`) is placed into register `a3`.
   * The Return Address is saved into register `a0`.
4. **The Jump**: The CPU executes `callx0 a8` (or `callx4`). The **Program Counter (`PC`)** register instantly updates to `0x400D5820`. The CPU is now reading instructions in `wifi_ui.c`!
5. **The Return**: When the callback hits `}`, it executes the `ret` opcode, loading `a0` back into `PC` and continuing the loop in `wifi_manager.c`.

### The Role of `void *user_data`
C functions do not have an implicit `this` pointer (C++) or `self` (Python). Passing `void *user_data` allows the caller to attach custom context (e.g. an LVGL widget handle `lv_obj_t*`, a FreeRTOS `QueueHandle_t`, or a state struct) without relying on fragile global variables.

---

## 8. Bidirectional Cloud Telemetry & Remote Command Execution Flow

The system runs a **dual-core decoupled architecture**:
* **Core 0 (Networking)**: FreeRTOS `mqtt_task` and `mqtt_telemetry_task` handle sockets, JSON parsing, and periodic broadcasts.
* **Core 1 (Graphics)**: Dedicated 60 FPS LVGL `gui_task` renders the UI and polls capacitive touch.

```
  [ Cloud Broker (EMQX) ]
        │  ▲
        │  │ (15s Periodic Telemetry JSON: Heap, Uptime, RSSI, Weather)
        │  │
        ▼  │
 ┌────────────────────────────────────────────────────────────────────────┐
 │ Core 0: mqtt_service.c & mqtt_telemetry_task                           │
 │                                                                        │
 │ 1. Inbound MQTT Packet received on esp32_iot/<MAC>/cmd                 │
 │ 2. MQTT_EVENT_DATA triggers handle_mqtt_command()                      │
 │ 3. cJSON parses {"cmd": "toast", "msg": "Hello!", "duration_ms": 4000}│
 │ 4. Sets cross-thread flags:                                            │
 │    • s_pending_toast_msg = "Hello!"                                    │
 │    • s_has_pending_toast = true                                        │
 │ 5. Publishes confirmation ACK to esp32_iot/<MAC>/response              │
 └───────────────────────────────────┬────────────────────────────────────┘
                                     │ Lockless Thread-Safe Flags
                                     ▼
 ┌────────────────────────────────────────────────────────────────────────┐
 │ Core 1: gui_task -> ui_periodic_timer() in wifi_ui.c                   │
 │                                                                        │
 │ 1. Fires every 200ms inside lv_timer_handler()                         │
 │ 2. Detects s_has_pending_toast == true                                 │
 │ 3. Unhides floating pill banner on lv_layer_top()                      │
 │ 4. Sets auto-dismiss timer (now_ms + 4000)                             │
 │ 5. Renders notification banner above all screens via SPI DMA!          │
 └────────────────────────────────────────────────────────────────────────┘
```

### Supported Remote Commands (JSON Payloads):
1. **Display Control**:
   * Sleep: `{"cmd": "display", "action": "sleep"}` (Turns off backlight, ST7796S Sleep In `0x10`).
   * Wake: `{"cmd": "display", "action": "wake"}` (Restores backlight, ST7796S Sleep Out `0x11`).
2. **Toast Notification Banner**:
   * `{"cmd": "toast", "msg": "Doorbell Rang at Front Gate!", "duration_ms": 5000}` (Displays floating pill alert on `lv_layer_top()`).
3. **Dynamic Weather Location**:
   * `{"cmd": "set_city", "name": "London", "lat": 51.5074, "lon": -0.1278}` (Updates Open-Meteo coordinates & triggers immediate REST fetch).
4. **On-Demand Ping**:
   * `{"cmd": "ping"}` (Responds with uptime and triggers immediate telemetry publish).
5. **OTA Firmware Update Trigger**:
   * `{"cmd": "ota", "url": "http://192.168.1.50:8000/firmware.bin"}` (Preparation gateway for Task 4).

---

## 9. Gateway to Task 4: Dual-Partition OTA (Over-The-Air) Firmware Updates

With Wi-Fi, SNTP, HTTPS REST, and MQTT remote control fully operational, the system has all required networking foundations to support remote firmware deployment:

* **The Trigger**: The MQTT `"cmd": "ota"` payload supplies the target binary URL.
* **The Storage**: The ESP32 partition table transitions from a single application slot to dual ping-pong partitions (`ota_0` and `ota_1`) with an active `otadata` boot-selector sector.
* **The Safeguard**: Hardware roll-back protection (`esp_ota_mark_app_valid_cancel_rollback`) ensures the device automatically recovers to the previous working firmware if a newly downloaded update crashes on boot.

---

*This document is maintained as the authoritative architectural reference in [`ESP_EVENT_AND_WIFI_TO_IP_DEEP_DIVE.md`](file:///c:/C_notes/ESP32_Bringup/ESP32_WIFI_IOT/ESP_EVENT_AND_WIFI_TO_IP_DEEP_DIVE.md).*
   