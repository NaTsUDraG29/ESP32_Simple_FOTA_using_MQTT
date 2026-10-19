# ESP32 Crash Forensics, Network Resilience & Low-Power Engineering

This guide provides an end-to-end engineering reference for diagnosing unexplained embedded crashes on the ESP32, designing production-grade Wi-Fi auto-reconnect sequences, and optimizing system power consumption through display sleep and CPU low-power modes.

---

## Part 1: How to Trace ESP32 Crashes When You "Have No Idea What Happened"

Embedded systems fail differently than desktop applications: there is no operating system popup or stack unwinding. Instead, the CPU hits an illegal hardware condition, triggers an architectural exception, and dumps the core registers to the UART console via the **Guru Meditation Error**.

### 1.1 Anatomy of a Guru Meditation Panic Dump

When an unhandled exception strikes, the ESP-IDF panic handler captures the register state at the exact instruction that failed:

```text
Guru Meditation Error: Core  1 panic'ed (LoadProhibited). Exception was unhandled.

Core  1 register dump:
PC      : 0x400e1072  PS      : 0x00060c30  A0      : 0x800e3e70  A1      : 0x3ffc73d0  
--- 0x400e1072: _lv_event_mark_deleted at .../lv_event.c:152
A2      : 0x3ffb8c08  A3      : 0x3ffb5c08  A4      : 0x00000001  A5      : 0x00000000  
A6      : 0x3ffb8c08  A7      : 0x3ffb5c08  A8      : 0x013f0000  A9      : 0x00000000  
A10     : 0x3ffb5c08  A11     : 0x3ffb8f54  A12     : 0x00000001  A13     : 0x00000000  
A14     : 0x00000000  A15     : 0x3ffb5c08  SAR     : 0x0000001d  EXCCAUSE: 0x0000001c  
EXCVADDR: 0x013f0004
```

Here is the exact meaning of every critical field:

| Register / Field | Technical Meaning | Diagnostic Rule of Thumb |
| :--- | :--- | :--- |
| **Core X panic'ed** | Identifies which core faulted (Core 0: PRO CPU / Wi-Fi; Core 1: APP CPU / GUI). | Tells you immediately which subsystem's task died. |
| **EXCCAUSE** | The numeric exception code raised by the Xtensa pipeline. | `0x1C` = `LoadProhibited`, `0x1D` = `StoreProhibited`, `0x00` = `IllegalInstruction`, `0x09` = `LoadStoreAlignment`. |
| **EXCVADDR** | **The Virtual Address that failed to be read or written.** | **The single most important diagnostic register!** Inspect its numeric value: |
| | `0x00000000` | Classic `NULL` pointer dereference (`ptr->field` when `ptr == NULL`). |
| | `0x00000004` to `0x00000040` | `NULL` pointer dereference of a struct member at an offset (e.g. `ptr->member` where `member` is 4 bytes inside the struct). |
| | `0x3FF80000` - `0x3FFFFFFF` | Legitimate internal SRAM address. If it faulted here, check memory write protection, stack bounds, or caching. |
| | `0x013F0004` or random garbage | **Pointer Corruption / Use-After-Free / Stack Leaks!** The pointer contains random memory bits. |
| **PC (Program Counter)** | The virtual memory address of the instruction that triggered the fault. | Points to the exact assembly opcode. `idf.py monitor` or `addr2line` maps this to `file.c:line`. |
| **A0** | Physical Return Address Register (`RA`). | Shows the caller of the current function (with the high bit masked for windowing). |
| **A1** | Task Stack Pointer (`SP`). | Current top of the stack. If close to the stack bottom, suspect stack overflow. |
| **A2 - A7** | Input Arguments & Function Return Values. | In the Xtensa ABI, `A2` is Argument 1 / Return value, `A3` is Argument 2, etc. |
| **A8 - A15** | Caller-saved working registers. | Often holds the base pointer of the struct being dereferenced. |

---

### 1.2 Forensic Dissection: Why Did Rescan Crash at `0x400e1072`?

Let's dissect the exact crash that occurred in our Wi-Fi rescan:

```text
PC      : 0x400e1072: _lv_event_mark_deleted at lv_event.c:152
EXCCAUSE: 0x0000001c (LoadProhibited)
EXCVADDR: 0x013f0004
A8      : 0x013f0000
```

#### Step 1: Look at the Source Code
In `lv_event.c:151-154`:
```c
void _lv_event_mark_deleted(lv_obj_t * obj)
{
    lv_event_t * e = event_head;

    while(e) {
        if(e->current_target == obj || e->target == obj) e->deleted = 1; // Line 152
        e = e->prev;
    }
}
```

#### Step 2: Correlate Assembly to Registers
At line 152, the Xtensa assembly translates `e->target` to:
`l32i.n a9, a8, 4` (Load 32-bit integer from memory address `A8 + 4` into register `A9`).
- `A8` held `0x013f0000` (which was `e`).
- `A8 + 4` was `0x013f0004` (`EXCVADDR`).
- The CPU tried to read memory at `0x013f0004`. Because `0x013f0004` is outside the ESP32's valid SRAM address space (`0x3FF80000 - 0x3FFFFFFF`), the hardware Memory Management Unit (MMU) threw a **`LoadProhibited`** exception!

#### Step 3: Why was `event_head` (`A8`) Garbage?
In LVGL, `event_head` is a linked list of active events:
```c
lv_res_t lv_event_send(lv_obj_t * obj, lv_event_code_t event_code, void * param)
{
    lv_event_t e;         // ALLOCATED ON THE CALL STACK!
    e.prev = event_head;
    event_head = &e;      // Global points to local stack frame!
    lv_res_t res = event_send_core(&e);
    event_head = e.prev;  // Restored when event finishes
    return res;
}
```
Notice that `e` is an **auto (stack) variable**!
When `gui_task` was spawned on Core 1 *before* `wifi_ui_init()` completed on Core 0:
1. Core 0 was running `wifi_ui_init()`, creating widgets and firing setup events. `event_head` was set to a pointer on Core 0's stack.
2. Simultaneously, Core 1 was running `gui_task` calling `lv_timer_handler()`.
3. When Core 0 returned from its function, its stack frame was destroyed and overwritten with other data (`0x013f0000`).
4. Core 1 then called `lv_obj_clean()`, which called `_lv_event_mark_deleted()`. It read the corrupted `event_head` pointer `0x013f0000` and crashed!

**The Fix**:
1. Call `wifi_ui_init()` strictly **before** spawning `gui_task` so all screens and widgets are fully created before the timer loop begins.
2. Protect `lv_timer_handler()` with a FreeRTOS recursive mutex (`lvgl_port_lock()` / `lvgl_port_unlock()`).
3. Enable `CONFIG_LV_MEM_CUSTOM=y` so LVGL allocations use the system heap instead of a cramped 32KB pool.

---

### 1.3 How to Read and Decode Backtraces

The backtrace in a panic dump is a list of Program Counter (`PC`) and Stack Pointer (`SP`) pairs representing the active call stack:

```text
Backtrace: 0x400e106f:0x3ffc73d0 0x400e3e6d:0x3ffc73f0 0x401853bf:0x3ffc7410 0x400e8ddc:0x3ffc7490 0x400de95b:0x3ffc74b0 0x400deab2:0x3ffc7750
```

To decode addresses manually when `idf.py monitor` is not running:
```powershell
xtensa-esp-elf-addr2line -pfia -e build/ESP32_WIFI_IOT.elf 0x400e106f 0x400de95b 0x400deab2
```
Output:
```text
0x400e106f: _lv_event_mark_deleted at lv_event.c:151
0x400de95b: wifi_ui_refresh_ap_list at wifi_ui.c:550
0x400deab2: ui_periodic_timer at wifi_ui.c:202
```

---

### 1.4 The 5 Root-Cause Categories in Embedded C

Whenever an unexpected crash occurs, it belongs to one of these five patterns:

```
                  ┌─────────────────────────────────────────┐
                  │    EMBEDDED CRASH ROOT CAUSES           │
                  └──────────────────┬──────────────────────┘
                                     │
     ┌──────────────────┬────────────┴───────┬──────────────────┐
     ▼                  ▼                    ▼                  ▼
┌──────────────┐ ┌──────────────┐    ┌──────────────┐   ┌──────────────┐
│ CONCURRENCY  │ │ STACK OVER-  │    │ HEAP/MEMORY  │   │ DANGLING     │
│ RACE CONDS   │ │ FLOW         │    │ EXHAUSTION   │   │ POINTERS     │
│ (No Mutex)   │ │ (HighWater=0)│    │ (malloc=NULL)│   │ (UseAfterFree│
└──────────────┘ └──────────────┘    └──────────────┘   └──────────────┘
```

1. **Concurrency Race Conditions**:
   - Two tasks on different cores (or an ISR and a task) accessing shared global state without a mutex.
   - *Detection*: Crash appears randomly during heavy network traffic or user input.
   - *Fix*: Decouple with FreeRTOS queues, event groups, or wrap in recursive mutexes.
2. **Stack Overflow**:
   - Calling deep nested functions, declaring large local arrays on stack (e.g. `char buf[4096]`).
   - *Detection*: `EXCVADDR` is just below `A1`, or `Stack canary watchpoint triggered`.
   - *Fix*: Check `uxTaskGetStackHighWaterMark()`. Allocate buffers in heap or `static`.
3. **Heap Exhaustion**:
   - Memory fragmentation or small static pools (e.g. LVGL 32KB internal pool).
   - *Detection*: `heap_caps_get_free_size(MALLOC_CAP_8BIT)` drops to near zero.
   - *Fix*: Enable `CONFIG_LV_MEM_CUSTOM=y` and monitor heap metrics.
4. **Dangling Pointers / Use-After-Free**:
   - Storing a pointer to an object that gets deleted later.
   - *Fix*: In LVGL, pass static slot buffers or clean event callbacks before object deletion.
5. **Memory Alignment Faults**:
   - Casting a `uint8_t*` buffer to a `uint32_t*` and dereferencing an unaligned address on architectures that enforce alignment.
   - *Fix*: Use `memcpy` or align attributes.

---

## Part 2: Production-Grade Wi-Fi Auto-Reconnection

### 2.1 Why Simple Wi-Fi Handlers Fail
A naive Wi-Fi handler connects on boot and handles `WIFI_EVENT_STA_DISCONNECTED` by only checking if the device was currently in the `CONNECTING` state.

If the router drops power, reboots, or suffers an RF fade while the device is in `WIFI_STATE_CONNECTED`, the event handler sees `state == CONNECTED`, classifies it as a generic disconnect, and never calls `esp_wifi_connect()` again. The device remains bricked in a disconnected state forever until power cycled.

### 2.2 The Resilience State Machine
A resilient Wi-Fi manager distinguishes between:
1. **User-Initiated Disconnect** (`wifi_manager_disconnect()`):
   - Sets `s_user_requested_disconnect = true`.
   - Disconnects cleanly and stays disconnected.
2. **Unexpected Link Loss** (Modem turned off, router reboot, beacon loss):
   - `s_user_requested_disconnect == false`.
   - Triggers an automatic retry sequence (up to 5 attempts).
   - Applies an exponential or small pacing backoff (1.5 seconds) to avoid spamming the radio.
   - Reports live retry feedback (`Wi-Fi link lost! Reconnecting (X/5)...`) to the UI.
   - If 5 retries fail, switches gracefully to the setup screen, displays `"Router unavailable"`, and enables the manual **Rescan** button.

```
       [CONNECTED] ──────── (Modem Powered Off / Link Lost) ────────┐
            │                                                       │
            │                                                       ▼
            │                                            [RECONNECTING (1/5)]
            │                                                       │
            │                                    (Fails, Backoff 1.5s)
            │                                                       ▼
            │                                            [RECONNECTING (2/5)...]
            │                                                       │
            │                                      (Fails 5 times)
            │                                                       ▼
            │                                            [CONNECT_FAILED]
            │                                                       │
            │                                           (Switch to Setup Screen,
            │                                            Show Error, Allow Rescan)
            │                                                       │
            └──────────────── (Link Restored) ◄─────────────────────┘
```

---

## Part 3: ESP32 Low-Power Modes & Display Inactivity Sleep

### 3.1 ESP32 Power Modes Comparison

| Mode | CPU State | Wi-Fi / BT | SRAM State | Typical Current | Wakeup Sources |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Active** | Both Cores at 160/240 MHz | Radio TX/RX active | Fully retained | **160 - 240 mA** | Running |
| **Modem Sleep** | Cores running (or dynamic frequency scaling 80-240 MHz) | Radio sleeps; wakes periodically for DTIM beacon | Fully retained | **20 - 30 mA** | Scheduled Wi-Fi DTIM beacon, RTOS ticks |
| **Light Sleep** | CPU clock gated; digital peripherals paused | Radio powered off | Fully retained | **~0.8 mA** | Touch pin, GPIO interrupt, RTC Timer, UART |
| **Deep Sleep** | CPU & digital peripherals completely powered off | Radio powered off | Main SRAM powered off; RTC memory retained | **10 - 150 µA** | RTC GPIO, RTC Timer, Touch Pad sensor, ULP Coprocessor |
| **Hibernation** | Entire chip powered off except 1 RTC timer | Powered off | All memory lost except 8KB RTC slow memory | **~5 µA** | RTC Timer only |

---

### 3.2 Display Power Architecture & Inactivity Sleep

On devices with color TFT displays, the **LCD backlight LEDs** consume dramatically more power than the ESP32 CPU itself!

#### Current Consumption Breakdown:
- **ESP32 Dual-Core (Active)**: ~80 mA (without Wi-Fi TX bursts).
- **ST7796S LCD Controller Logic**: ~15 mA.
- **LCD Backlight LEDs (480×320 TFT)**: **~180 mA!**
- *Total Active Power*: **~275 mA**.

By turning off the backlight and placing the panel into sleep mode:
- Backlight current drops from 180 mA to **0 mA**.
- ST7796S panel controller drops from 15 mA to **~0.1 mA** (via Sleep In command `0x10`).
- **Total System Current drops by over 70%!**

```
Active Mode:   [████████████████████████████████] 275 mA (180 mA is Backlight alone!)
Display Sleep: [████████] 80 mA (Backlight completely OFF, CPU maintaining Wi-Fi)
```

#### Hardware Control Mechanism:
1. **Sleep In (`lcd_sleep()`)**:
   - Sends SPI command `0x10` (`SLPIN` - Sleep In). The ST7796S halts its internal clock oscillators, gate driver circuits, and DC-DC step-up converters.
   - Clears Bit 5 of the **TCA9534 I/O expander** output port register (`0b01000110`), cutting base current to the Q2 NPN transistor that drives the backlight LED cathode.
2. **Wakeup (`lcd_wakeup()`)**:
   - Sets Bit 5 of TCA9534 (`0b01100110`), illuminating the backlight LEDs.
   - Sends SPI command `0x11` (`SLPOUT` - Sleep Out) and waits 10 ms for the internal power circuits to stabilize.
   - Sends SPI command `0x29` (`DISPON` - Display ON).
3. **Seamless Tap-to-Wake with GT911**:
   - The GT911 capacitive touch controller operates continuously over I2C.
   - When the user touches the sleeping screen, the GT911 detects the capacitive disturbance and signals `gt911_lvgl_read_cb()`.
   - `wifi_ui_notify_activity()` immediately calls `lcd_wakeup()`.
   - **Critical UX Detail**: The touch callback inspects `wifi_ui_is_display_sleeping()`. If the screen was sleeping, it suppresses the press event (`LV_INDEV_STATE_REL`) for that initial tap. This guarantees that touching the screen to wake it up **never accidentally presses a button** that happened to be underneath the user's finger!
