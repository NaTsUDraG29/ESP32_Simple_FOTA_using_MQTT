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

## 5. Summary: The Complete System Design Picture

| Stage | Responsible Layer & Task | What Physical Action Occurs? | What Event Is Emitted? | Who Receives It? |
| :--- | :--- | :--- | :--- | :--- |
| **1. Connect** | App Task (`main`) | Calls `esp_wifi_connect()` | *None* | Wi-Fi Driver Task |
| **2. Radio Association** | Wi-Fi Driver Task (`wifi`, Core 0) | 802.11 Auth, Assoc, & 4-Way WPA2 Handshake | **`WIFI_EVENT_STA_CONNECTED`** | 1. Your `wifi_event_handler`<br/>2. `esp_netif` internal callback |
| **3. DHCP Negotiation** | TCP/IP Task (`tiT` / LwIP, Core 0) | Broadcasts DHCP Discover $\to$ Router replies with IP lease | *None (UDP packets on wire)* | Router DHCP Server |
| **4. IP Acquisition** | TCP/IP Task (`tiT` / LwIP, Core 0) | Commits IP `192.168.1.105` to network interface | **`IP_EVENT_STA_GOT_IP`** | Your `wifi_event_handler` |
| **5. Application Response** | Event Loop Task (`sys_evt`, Core 0) | Executes your callback: updates UI, saves NVS, triggers SNTP | *None* | `gui_task` (Core 1) & SNTP |

---

*This document is saved permanently in your workspace at [`ESP_EVENT_AND_WIFI_TO_IP_DEEP_DIVE.md`](file:///c:/C_notes/ESP32_Bringup/ESP32_WIFI_IOT/ESP_EVENT_AND_WIFI_TO_IP_DEEP_DIVE.md).*



┌──────────────────────────────────────────────┐
│  ESP-IDF Wi-Fi Driver (Hardware / Event Loop) │  <-- Reports Hardware State
└──────────────────────┬───────────────────────┘
                       │ "Hardware RF is ON, PHY calibrated, MAC is ready"
                       ▼ (WIFI_EVENT_STA_START)
┌──────────────────────────────────────────────┐
│  State Machine: set_state(WIFI_STATE_IDLE)   │  <-- Device is READY for commands
└──────────────────────────────────────────────┘
                       │
                       │ What should we do now?
                       │ (Scan? Auto-connect? Sleep? SoftAP?)
                       ▼
┌──────────────────────────────────────────────┐
│  Application Business Logic (app_main)       │  <-- MAKES THE DECISION!
└──────────────────────────────────────────────┘



 1. Application Layer (app_main or Rescan Button)
    │
    ▼ calls
 2. wifi_manager_start_scan()
    │
    ├──► set_state(WIFI_STATE_SCANNING)
    │    └──► UI shows amber status: "Scanning 2.4 GHz channels..." + Spinner
    │
    └──► esp_wifi_scan_start(&scan_config, false); (non-blocking)
         │
         ▼ (Background Hardware RF Operation)
 3. ESP32 Wi-Fi Task hops across channels 1 to 13:
    - Broadcasts 802.11 Probe Requests
    - Listens for Beacons & Probe Responses from nearby routers
    - Captures SSID, RSSI (dBm), and Authentication type (WPA2/WPA3/Open)
         │
         ▼ (Scan finishes on channel 13)
 4. Hardware driver posts event: WIFI_EVENT_SCAN_DONE
    │
    ▼ (Received by wifi_event_handler in wifi_manager.c)
 5. wifi_event_handler() handles case WIFI_EVENT_SCAN_DONE:
    ├──► esp_wifi_scan_get_ap_records() copies raw records into s_scanned_aps[]
    └──► set_state(WIFI_STATE_SCAN_DONE)
         │
         ▼ (Callback triggers on_wifi_state_changed in wifi_ui.c)
 6. on_wifi_state_changed(WIFI_STATE_SCAN_DONE):
    ├──► s_pending_spinner = 0 (Hide spinner)
    ├──► s_pending_scan_refresh = true (Signal Core 1 GUI task)
    └──► wifi_ui_set_status("Scan complete. Tap a network to connect.")
         │
         ▼ (Core 1 timer tick runs ui_periodic_timer)
 7. Core 1 gui_task executes ui_periodic_timer():
    └──► Sees s_pending_scan_refresh == true
         └──► Calls wifi_ui_refresh_ap_list()
              - Populates list buttons on the LCD with SSIDs & signal strength (dBm)!