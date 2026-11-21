## Web UI/UX
- Video: https://github.com/xiv3r/esp32-4in1-carwash/releases/download/v1.0/4in1carwash.mp4

## Table of Contents

1. [What This Controller Does](#1-what-this-controller-does)
2. [Parts You Need](#2-parts-you-need)
3. [Wiring Reference](#3-wiring-reference)
4. [Power and Grounding](#4-power-and-grounding)
5. [First Power-Up](#5-first-power-up)
6. [Connecting to the Web Panel](#6-connecting-to-the-web-panel)
7. [Everyday Operation (Customer Flow)](#7-everyday-operation-customer-flow)
8. [Button Gestures](#8-button-gestures)
9. [Reading the LCD](#9-reading-the-lcd)
10. [Coin and Bill Acceptor Setup](#10-coin-and-bill-acceptor-setup)
11. [Time Models — How Credit Is Spent](#11-time-models--how-credit-is-spent)
12. [Bonus Time (Long-Press)](#12-bonus-time-long-press)
13. [The Web Panel Page by Page](#13-the-web-panel-page-by-page)
14. [Settings Reference (All Values and Ranges)](#14-settings-reference-all-values-and-ranges)
15. [API Reference (Advanced)](#15-api-reference-advanced)
16. [Statistics and Sales Tracking](#16-statistics-and-sales-tracking)
17. [Backup, Restore and Cloning](#17-backup-restore-and-cloning)
18. [Maintenance and Safety Systems](#18-maintenance-and-safety-systems)
19. [Factory Reset and PIN Recovery](#19-factory-reset-and-pin-recovery)
20. [Troubleshooting](#20-troubleshooting)
21. [Developer Notes](#21-developer-notes)
---

## 1. What This Controller Does

This controller turns an ESP32 into the brain of a self-service carwash bay:

- **Four coin-operated functions** — WATER, BLOWER, SOAP and FOAM — each with its own customer push button and relay output for the pump, blower or valve behind it.
- **Time-pool vending** — customers insert coins or bills; the acceptor's pulses convert to minutes of credit; functions run only while credit remains.
- **Built-in Wi-Fi panel** — the board broadcasts its own access point with a captive web interface (no app, no internet). From any phone or laptop you can watch live status, start/stop functions, change pricing and read sales reports.
- **20×4 LCD status display** showing all four functions at once, plus a buzzer for coin/start/error feedback.
- **Persistent storage** — pricing, pins, Wi-Fi and statistics survive power cuts (stored in flash with dual-slot redundancy and CRC checks).
- **Self-protection** — run-time caps, a low-memory watchdog, flash-wear control, and a self-healing access point.

The machine keeps working even if the LCD, the buzzer or Wi-Fi clients are absent — each subsystem is probed and recovered independently.

## 2. Parts You Need

| Part | Purpose | Notes |
|---|---|---|
| ESP32 38-pin dev board | The controller | Any 38-pin variant; firmware is not board-brand specific |
| 4-channel relay module (opto-isolated) | Switches pumps/blower/valves | Active-low trigger assumed by default |
| 4× momentary push buttons | Customer function selectors | Wire pin → button → GND; internal pull-ups, no resistors needed |
| Coin or bill acceptor with pulse output | Vending input | NPN/open-collector or relay-style pulse output |
| 20×4 I2C LCD (backpack at 0x27 or 0x3F) | Status display | 5 V supply |
| Active buzzer or speaker + transistor | Audible feedback | Drive through a transistor if it draws > 20 mA |
| 5 V power supply (≥ 2 A) | Everything | ESP32, LCD, relay module coils |
| Hook-up wire, terminals, enclosure | Assembly | — |

## 3. Wiring Reference

Default pin map for this v2 build. **Every pin is reassignable** in the web panel (Settings → Buttons & Relays); the controller rejects configurations with duplicate pins or pins that are unsafe on the ESP32, so you cannot burn a config that double-books a GPIO.

| Connection | GPIO | Notes |
|---|---|---|
| WATER relay | **19** | Relay module input IN1 |
| BLOWER relay | **18** | Relay module input IN2 |
| SOAP relay | **17** | Relay module input IN3 |
| FOAM relay | **16** | Relay module input IN4 |
| WATER button | 32 | Button connects GPIO to GND |
| BLOWER button | 33 | Button connects GPIO to GND |
| SOAP button | 27 | Button connects GPIO to GND |
| FOAM button | 14 | Button connects GPIO to GND |
| Coin/bill acceptor pulse | 25 | FALLING-edge pulse input, pull-up |
| Pulse output (mirror) | 26 | Optional copy of accepted pulses for external counters (60 ms wide, configurable 10–1000 ms) |
| Buzzer | 23 | `tone()` driven |
| LCD SDA | 21 | I2C data |
| LCD SCL | 22 | I2C clock |
| BOOT button (on the ESP32 board itself) | 0 | Hold 5 s for factory reset |

> **v1 → v2 wiring alert:** the relay defaults moved. v1 used WATER 13 / BLOWER 16 / SOAP 17 / FOAM 18. v2 uses **WATER 19 / BLOWER 18 / SOAP 17 / FOAM 16**. A v1 config saved in flash keeps its old pins after a v2 flash — only a **factory-fresh** v2 unit uses the new map. Check the Pin Map card (Settings → Buttons & Relays) after upgrading.

**Pins the firmware allows for assignment:** 4, 5, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 only. The BOOT pin (0) is fixed by validation and cannot be remapped. Input-only pins (34–39) and strapping pins (0, 2, 12) outside that list are deliberately excluded.

## 4. Power and Grounding

1. **5 V rail:** feed the ESP32 (VIN/5V), the LCD and the relay module from one clean 5 V supply rated at least 2 A. The ESP32 alone spikes above 500 mA at boot and Wi-Fi transmit; relay coils add ~70 mA each.
2. **Common ground is mandatory:** ESP32 GND, relay module GND, acceptor GND and LCD GND must all connect together. A missing common ground is the **number one cause of coins not being counted** — the pulse has no return path.
3. **Motor/pump loads:** relays switch the *load side* only. Never share the pump's power wiring with the logic supply; run pump power through the relay contacts from its own breaker.
4. **Voltage sag check:** if the board brown-outs (repeated surprise reboots) when a pump kicks in, the 5 V rail is sagging. Use a bigger supply or add bulk capacitance (470–1000 µF) at the relay module.

## 5. First Power-Up

1. **Apply power.** The buzzer plays a short boot jingle; the LCD shows a splash for 3 seconds with the Wi-Fi name and panel address. All relays stay OFF.
2. **Join the Wi-Fi.** On a phone or laptop connect to:
   - **Network:** `ESP32_4in1_Carwash`
   - **Password:** `12345678`
3. **Open the panel.** The captive portal usually pops up by itself. If not, browse to **http://192.168.4.1** — the dashboard loads live status within a second.
4. **Secure the panel (recommended).** Settings → General → set a **Panel PIN** (digits only, up to 8 characters). Once set, every write action (start/stop, config save, reset) asks for it. Without a PIN, anyone who joins the Wi-Fi has full control.
5. **Check the time model.** This v2 build defaults to **Allocated per function** — insert a coin, press WATER, and the whole credit moves into WATER. If you want several functions to run at once on one credit, switch to a shared-pool model first (Settings → General → Time model). See [§11](#11-time-models--how-credit-is-spent).
6. **Test without coins.** Settings → Coin has **Simulate 1 / 5 / 10 / 20 pulse** buttons that feed the acceptor logic directly. Simulate a coin, watch the LCD and dashboard update, then press a function button to confirm relay + buzzer + LCD all respond.
7. **Check relay polarity.** If relays are ON while idle, flip **Settings → Buttons & Relays → "Active low module"** to match your relay board.

The access point is self-healing: it is verified every 5 seconds and restarted automatically if it ever disappears. Up to 4 simultaneous clients are allowed by default (configurable 1–8).

## 6. Connecting to the Web Panel

| Item | Default |
|---|---|
| SSID | `ESP32_4in1_Carwash` |
| Password | `12345678` |
| Panel URL | `http://192.168.4.1` |
| Channel | 1 (configurable 1–13) |
| Hidden network | Off (configurable) |
| Max clients | 4 (configurable 1–8) |

- The panel works on phones and desktops, in light or dark theme (theme is remembered per browser).
- Captive-portal detection is built in for Android (`generate_204`), Apple (`hotspot-detect.html`) and Windows (`connecttest.txt` / `ncsi.txt` etc.), so the login window usually opens automatically on join.
- The panel needs no internet and never phones home — everything stays on the local network.
- Password rules: 8–63 printable ASCII characters (or empty for an open network — not recommended). Saving new credentials restarts the access point within about a second; reconnect with the new name/password.

## 7. Everyday Operation (Customer Flow)

1. Customer inserts a coin (or bill) into the acceptor.
2. The burst of pulses is decoded; on completion the buzzer beeps and the LCD shows for 2.5 seconds: pulse count, credit added, and the running total.
3. Customer presses a function button once → that function starts (two short beeps).
   - In the default **allocated** model, the **entire credit balance moves into that function** and the other buttons stay inactive until it stops.
   - In a **shared** model, several functions can run at once from the same pool.
4. LCD rows update live; a paused function blinks its timer as a reminder.
5. When credit runs out, every running function stops automatically and its relay releases. An unfinished pause simply holds its state until credit returns or the run cap expires.

**What is never lost:** unused paid time. In shared mode it stays in the pool; in allocated mode the unused remainder returns to the pool when the function stops (button stop, timeout, or model change).

**What *is* lost on reboot:** remaining credit. The pool lives in RAM by design (flash-wear protection); any restart — power cut, manual reboot, watchdog — clears it to zero. Saved statistics are committed at most every 5 minutes, so use *Commit statistics now* (Settings → System) before planned shutdowns.

## 8. Button Gestures

Each function button supports three gestures (timings configurable in Settings → General):

| Gesture | Default timing | Action |
|---|---|---|
| Single press | — | Start the function; resume if paused; **in allocated mode, first top up that function from the pool (up to the credit cap)** |
| Double press | within 350 ms | Pause a running function (relay off, time kept); double press again to resume |
| Long press | ≥ 900 ms | Add bonus time (default +60 s) if bonus is enabled — see [§12](#12-bonus-time-long-press) |

Extra behaviours baked in:

- Starting with no credit available → error beep, nothing runs.
- Pressing a button while that function is already running → short click beep (no state change; use double press to pause).
- Pausing a function never refunds time; the pause simply freezes consumption.
- The buzzer gives distinct patterns for coin accepted, start, pause, stop, error and factory-reset events (patterns can be tested one by one in Settings → Buzzer).

## 9. Reading the LCD

Each row is `NAME time STATE` (20×4 default):

```
WATER 12:34 RUN
BLOWER --:-- OFF
SOAP   --:-- REDY
FOAM   --:-- PAUS
```

| State | Meaning |
|---|---|
| `RUN` | Function running, counting down |
| `PAUS` | Paused by double-press, time held |
| `REDY` | Credit available, function idle, ready to start |
| `OFF`  | No credit in the pool |

- **Paused functions blink their timer** (on/off at the configured blink rate, default 500 ms) so the customer sees what is on hold. The blinking stops when the function resumes. *(v1 blinked the last-selected running function instead.)*
- Accepted coins take over the screen for 2.5 s (pulses, credit added, running total), then the four rows return.
- Overlay messages (factory-reset countdown, etc.) temporarily replace rows and time out by themselves.
- A disconnected LCD never blocks operation: the I2C bus is re-probed every 15 seconds and the display resumes when reconnected.

## 10. Coin and Bill Acceptor Setup

### How the decoder works

1. Each falling edge on GPIO 25 counts as one pulse (pull-up on, so open-collector acceptors work directly).
2. Pulses closer than **30 ms** apart are treated as contact bounce and ignored (`coinMinPulseGapMs`).
3. A coin "group" completes after **350 ms** of silence following the last pulse (`coinGroupGapMs`).
4. The completed group is matched against your presets. A group larger than **60 pulses** is capped as a safety limit (`coinMaxPulses`).

### Default pricing table (4 of 8 presets used)

| Pulses | Time granted | Sales value | Typical coin |
|---:|---|---:|---|
| 1 | 5 minutes | ₱1.00 | ₱1 |
| 5 | 25 minutes | ₱5.00 | ₱5 |
| 10 | 50 minutes | ₱10.00 | ₱10 |
| 20 | 1 hour (60 min) | ₱20.00 | ₱20 |

- Up to **8 presets** can be defined (Settings → Coin). Pulse counts must be **unique** — validation rejects duplicates.
- **Prorating:** a group matching no preset exactly (e.g. 7 pulses) is *not* rejected. It takes the nearest lower preset (5 pulses / 25 min / ₱5) and adds the per-pulse rate for the extra pulses → 35 min, ₱7. Multi-coin drops still credit fairly.
- **Configure your acceptor's pulse output** (via its own programming manual or DIP switches) so each coin/bill emits a distinct pulse count that matches a preset exactly — that avoids prorating rounding.

### Acceptor wiring

| Acceptor wire | Goes to |
|---|---|
| Pulse (COIN/sign/signal) | GPIO 25 |
| GND (0 V) | ESP32 GND (common ground!) |
| +12 V (if required) | Acceptors usually need their own 12 V supply — grounds tied together |

### Testing tools (Settings → Coin)

- **Pulse monitor** — live view of pulses as they arrive.
- **Simulate 1 / 5 / 10 / 20** — inject a fake coin without feeding the acceptor.
- **Owner credit tool** — add seconds manually (counts as credit added, *not* as sales).
- **Pulse output (GPIO 26)** — mirrors every accepted pulse (60 ms wide) for an external counter or second validator; can be disabled to free the pin.

## 11. Time Models — How Credit Is Spent

The time model (Settings → General) decides how the credit pool drains while functions run. It can be changed at any time; active sessions close cleanly and unused time returns to the pool.

| Model | Behaviour | Best for |
|---|---|---|
| **Shared pool · single rate** | Any mix of functions can run together; pool drains 1 s per second *in total*. Two hours of credit lasts two hours however many functions run. | Simplest customer understanding; consumables cost roughly the same across functions |
| **Shared pool · per-function drain** | Pool drains 1 s per second **per running function** — two functions running burns credit twice as fast. | Electricity/consumables cost about the same per function |
| **Allocated per function** *(v2 default)* | Starting a function moves the **whole** credit balance into that function; other buttons stay OFF until it stops or credit runs out. Unused time returns to the pool on stop. A single press on a running function tops it back up from the pool, up to the credit cap. | Stage-by-stage washing: water first, then foam, then blower |

Safety caps that apply regardless of model:

- **Credit cap** — the pool can never exceed 35,999 s (~10 h) (`maxCreditSeconds`, range 60–86400).
- **Run cap** — any function still running after 2 continuous hours is force-stopped (`maxRunSeconds`, range 60–86400, default 7200).

## 12. Bonus Time (Long-Press)

Long-pressing a function button adds bonus time (default +60 s) to that function — a common marketing feature ("hold for free extra shampoo time").

- **Enabled by default**; can be disabled in Settings → General.
- **Budget cap:** at most 600 s (10 min) of bonus per credit cycle (`maxBonusSeconds`).
- **The budget resets each time a coin is accepted**, so long-press abuse can never accumulate unlimited free time — every fresh coin buys a fresh 10-minute bonus allowance.
- Bonus seconds are tracked separately in statistics (`bonusSeconds`), so you can see how much free time you are giving away.
- In allocated mode, bonus fills the channel's remaining-time balance (capped); in shared modes it fills the pool.

## 13. The Web Panel Page by Page

Nine pages, reached from the left sidebar:

| Page | What you do there |
|---|---|
| **Dashboard** | Live credit pool, active functions, all-time revenue/coins/minutes KPIs, per-function Start / Pause / Stop / +1 min buttons, Start-all / Stop-all, 14-day sales chart, LCD mirror, system status (Wi-Fi, clients, heap, LCD), recent sessions table |
| **Sales Inventory** | Revenue, coins, sessions, bonus time, average ticket, runtime per function, daily ledger (32-day ring), pricing table, CSV export |
| **Settings · General** | Time model, long-press bonus, run/credit caps, save interval, factory-hold time, Panel PIN, low-heap guard |
| **Settings · Coin** | Acceptor pins, pulse timing, up to 8 price presets, pulse simulator, owner credit tool |
| **Settings · LCD** | I2C address (0x27 / 0x3F / 0x20 / 0x38 presets), size, pins, backlight, refresh/blink rates, live mirror |
| **Settings · Buttons & Relays** | All 8 GPIO pins, relay polarity, full live pin map |
| **Settings · Buzzer** | Enable/disable, pin, tone frequencies, per-pattern test buttons |
| **Settings · Wi-Fi AP** | Network name, password, channel, hidden mode, max clients, live client count |
| **Settings · System** | Diagnostics, config backup/restore, commit statistics, restart, reset statistics, factory reset |

**How saving works:** each settings page has its own Save button; values are validated *by the controller* before storage. An out-of-range value or pin conflict is rejected with the offending field's name shown in the UI. Good values are written to two alternating flash slots with a CRC, so even a power cut mid-write cannot lose your configuration — the previous good copy is used. The panel then **waits for the controller to confirm** the new configuration revision before reporting success; if the controller is busy with another save it answers "busy" (HTTP 503) and the panel tells you to retry.

**PIN protection:** once a Panel PIN is set, every write action prompts for it (the browser remembers it for the session). Read-only pages (Dashboard, Diagnostics) stay viewable without a PIN so attendants can monitor.

## 14. Settings Reference (All Values and Ranges)

All defaults and allowed ranges, as enforced by `validateConfig()`:

### General

| Setting | Default | Range | Meaning |
|---|---:|---|---|
| `timeModel` | 2 (allocated) | 0–2 | See [§11](#11-time-models--how-credit-is-spent) |
| `longPressAddSeconds` | 60 | 0–3600 (must be > 0 if bonus enabled) | Bonus seconds per long press |
| `maxBonusSeconds` | 600 | 0–86400 | Bonus budget per credit cycle |
| `maxRunSeconds` | 7200 | 60–86400 | Force-stop cap per continuous run |
| `maxCreditSeconds` | 35999 | 60–86400 | Maximum pool size |
| `statsSaveIntervalSec` | 300 | 30–3600 | Statistics commit cadence |
| `factoryHoldMs` | 5000 | 1000–30000 | BOOT-hold time for factory reset |
| `adminPin` | *(empty)* | 0–8 digits | Panel PIN |
| `lowHeapRestart` | 1 (on) | 0/1 | Low-heap watchdog enable |
| `lowHeapBytes` | 12288 | 4096–131072 | Low-heap threshold |

### Coin acceptor

| Setting | Default | Range | Meaning |
|---|---:|---|---|
| `coinPin` | 25 | allowed pin list | Pulse input |
| `coinMinPulseGapMs` | 30 | 5–500 | Debounce gap |
| `coinGroupGapMs` | 350 | 50–5000, ≥ gap+20 | Silence that ends a group |
| `coinMaxPulses` | 60 | 1–200 | Safety cap per group |
| `coinMapCount` | 4 | 1–8 | Active presets |
| preset pulses | 1/5/10/20 | 1–200, unique | Pulses per coin/bill |
| preset minutes | 5/25/50/60 | 1–1000 | Time granted |
| preset value | 100/500/1000/2000 | 0–1000000 centavos | Sales value |
| `pulseOutEnabled` | 1 | 0/1 | Mirror output |
| `pulseOutPin` | 26 | allowed pin list | Mirror output pin |
| `pulseOutWidthMs` | 60 | 10–1000 | Mirror pulse width |

### Buttons

| Setting | Default | Range | Meaning |
|---|---:|---|---|
| `debounceMs` | 30 | 5–200 | Contact debounce |
| `doubleClickMs` | 350 | 100–2000 | Double-press window |
| `longPressMs` | 900 | 200–5000 | Long-press threshold |
| `btnPin[4]` | 32/33/27/14 | allowed pin list | Function buttons |

### Relays

| Setting | Default | Range | Meaning |
|---|---:|---|---|
| `relayPin[4]` | **19/18/17/16** | allowed pin list | Relay outputs |
| `relayActiveLow` | 1 (active low) | 0/1 | Trigger polarity |

### LCD

| Setting | Default | Range | Meaning |
|---|---:|---|---|
| `lcdAddr` | 0x27 | 0x08–0x77 | I2C address |
| `lcdCols` | 20 | 16–20 | Columns |
| `lcdRows` | 4 | 2–4 | Rows |
| `lcdSda` / `lcdScl` | 21 / 22 | allowed pin list | I2C pins |
| `lcdBacklight` | 1 | 0/1 | Backlight |
| `lcdRefreshMs` | 250 | 50–2000 | Update rate |
| `lcdBlinkMs` | 500 | 120–2000 | Paused-timer blink rate |

### Buzzer

| Setting | Default | Range | Meaning |
|---|---:|---|---|
| `buzzerEnabled` | 1 | 0/1 | On/off |
| `buzzerPin` | 23 | allowed pin list | Output pin |
| `buzzerFreq` | 2400 Hz | 100–10000 | Normal beep |
| `buzzerErrorFreq` | 700 Hz | 100–10000 | Error beep |
| `buzzerBeepMs` | 70 | 10–1000 | Beep length |

### Wi-Fi AP

| Setting | Default | Range | Meaning |
|---|---|---|---|
| `apSsid` | `ESP32_4in1_Carwash` | 1–32 printable chars | Network name |
| `apPass` | `12345678` | 8–63 chars or empty | Password |
| `apChannel` | 1 | 1–13 | RF channel |
| `apHidden` | 0 | 0/1 | Hide SSID |
| `apMaxClients` | 4 | 1–8 | Simultaneous clients |

## 15. API Reference (Advanced)

For custom integrations (home automation, kiosk software, scripts). Base URL `http://192.168.4.1`. Write endpoints require the PIN (if set) via `X-Pin` header or `pin` form field. Request bodies are line-oriented `key=value` text (one pair per line).

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/ping` | GET | Liveness check |
| `/api/state` | GET | Live state (pool, channels, LCD lines, AP status, config revision) |
| `/api/stats` | GET | Full statistics (revenue, coins, days, sessions) |
| `/api/diag` | GET | Diagnostics (heap, resets, pulse counters, config revision, pins) |
| `/api/config` | GET | Current configuration |
| `/api/config` | POST | Apply a configuration (validated; returns new `cfgRevision`; "busy" 503 if a save is already in flight) |
| `/api/config/defaults` | POST | Reset configuration to factory defaults (keeps stats) |
| `/api/backup` | GET | Download configuration as a JSON file |
| `/api/restore` | POST | Restore a backup (same handler as config POST) |
| `/api/channel` | POST | Control a function: `ch=0..3`, `action` = start / pause / resume / stop / toggle / add, optional `seconds` (≤ 3600) |
| `/api/credit` | POST | Add owner credit: `seconds` (1–86400) |
| `/api/coin` | POST | Simulate coin: `pulses` (1–200) |
| `/api/buzzer` | POST | Test buzzer: `pattern` (1–10) |
| `/api/stats/reset` | POST | Reset statistics (requires `confirm=RESET`) |
| `/api/stats/save` | POST | Commit statistics to flash now |
| `/api/sessions/clear` | POST | Clear session journal (requires `confirm=CLEAR`) |
| `/api/factory-reset` | POST | Full factory reset (requires `confirm=FACTORY`) |
| `/api/reboot` | POST | Clean restart (requires `confirm=REBOOT`) |

Example — start the WATER channel from a script:

```bash
curl -X POST http://192.168.4.1/api/channel \
  -d "ch=0" -d "action=start" -d "pin=1234"
```

All responses are JSON. Errors come back as `{"ok":0,"error":"<field>"}` with HTTP 400/401/413/503 as appropriate. Write commands are **queued and executed by the control loop** (reboot and factory reset execute ~0.7 s later, statistics resets ~0.12 s later), so an "ok" reply means *queued*, not yet applied — poll `/api/state` (`cfgRevision`, pool, channel states) to confirm.

## 16. Statistics and Sales Tracking

The controller tracks, and never forgets across reboots:

- **Revenue** (in centavos, from preset values), **coins accepted**, **pulses counted**
- **Bonus seconds given away** (separate from paid time)
- **Runtime and session counts per function**
- **Daily ledger** — a 32-day ring of per-day revenue/coins/seconds (viewed on the Sales Inventory page with the 14-day chart)
- **Session journal** — the last 16 wash sessions with channel, duration and end reason (1 = credit exhausted, 2 = run-cap limit, 3 = resume with no credit, 4 = stopped, 5 = model change)

Notes:

- Statistics are committed to flash every 5 minutes by default (configurable) to protect flash lifespan. Use **Commit statistics now** before powering down.
- **Reset statistics** (Settings → System) clears revenue/coins/days/sessions but keeps total uptime and boot count.
- The **Sales Inventory** page exports the ledger and per-function totals as CSV for bookkeeping.
- Simulated coins count as **credit added but not as revenue**; owner credit likewise. Only real acceptor coins with preset values add revenue.

## 17. Backup, Restore and Cloning

1. **Download:** Settings → System → *Download configuration* → saves a small text file (`key=value` lines, `carwash-config.txt`) with every setting.
2. **Restore:** Settings → System → *Restore configuration* → pick the file → the controller validates and applies it, then the panel waits for the revision confirmation. Restoring a backup from another unit is the fastest way to clone a fleet of machines.
3. The backup contains the Wi-Fi credentials and PIN too — store it securely.
4. Configuration history is protected by the dual-slot scheme: every save alternates between two flash slots with a CRC; a corrupt or interrupted write falls back to the previous good copy automatically. The active slot and total writes are visible in Diagnostics.

## 18. Maintenance and Safety Systems

| System | Behaviour |
|---|---|
| **Run cap** | Any function running longer than `maxRunSeconds` (default 2 h) is force-stopped to protect pumps and motors |
| **Low-heap watchdog** | If free memory stays below 12 kB (or the largest free block below 6 kB) across four 30-second checks: relays off → statistics saved → restart. Visible as "low heap strikes" in Diagnostics |
| **Flash-wear control** | Statistics written at most every `statsSaveIntervalSec` (default 300 s); configuration only on explicit saves |
| **AP self-heal** | Access point verified every 5 s, restarted automatically if missing; AP restart and Wi-Fi failure counters visible in Diagnostics |
| **Dual-slot config** | Two alternating flash slots + CRC; a bad write falls back to the last good copy |
| **Session-ring guard** | Statistics loaded from flash are checked for sane journal pointers; corrupt data falls back to defaults instead of crashing |
| **LCD hot-plug** | Missing display never blocks operation; bus re-probed every 15 s |
| **Captive portal** | Full redirect set for Android/Apple/Windows detection |
| **Clean restart** | System → Restart performs: relays off → statistics committed → reboot |

**Recommended routine:** monthly — check Diagnostics for low-heap restarts and brownouts (power issues), test each relay by hand from the Dashboard, verify coin counting with a real coin, and export a fresh config backup + statistics CSV.

## 19. Factory Reset and PIN Recovery

A factory reset erases **everything** — configuration, statistics, session journal, Wi-Fi credentials — and reboots with the defaults from this guide (network `ESP32_4in1_Carwash` / `12345678`, **v2 default pins** WATER 19 / BLOWER 18 / SOAP 17 / FOAM 16, **allocated time model**, default pricing).

Two ways to trigger it:

1. **Web panel:** Settings → System → *Factory reset* (danger zone; asks for typed confirmation).
2. **BOOT button:** with the machine powered and boot complete, **hold the BOOT button for 5 seconds**. The LCD shows a countdown; releasing early aborts. At 5 s the controller plays the factory jingle, shows "FACTORY RESET / ERASING STORAGE / RESTARTING", wipes NVS and reboots.

**Forgot the Panel PIN?** The BOOT-button reset is the recovery path — there is no PIN bypass by design. After the wipe, set a new PIN and restore your configuration backup to be running again in a couple of minutes.

**Reset statistics only** (keep settings): Settings → System → *Reset sales statistics*.

## 20. Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| **Coins not counted** | 1) Missing common ground between acceptor and ESP32 — the #1 cause. 2) Pulse wire not on GPIO 25. 3) Check "dropped coin pulses" in Diagnostics; if pulses arrive but are dropped, increase `coinMinPulseGapMs`. 4) Bill validators sending long bursts — raise `coinGroupGapMs` so the group can finish. |
| **Relays inverted (ON when idle)** | Relay board trigger polarity mismatch — toggle "Active low module" in Settings → Buttons & Relays. |
| **Wrong relay fires** | You upgraded v1→v2 firmware and expected the new pin map — a saved v1 config keeps its old pins. Check the Pin Map card, or factory reset to adopt v2 defaults. |
| **Only one function works at a time** | That is the v2 default **allocated** model working as designed. Switch the time model to a shared pool (Settings → General) to run several functions on one credit. |
| **LCD blank** | 1) Check 5 V supply. 2) SDA/SCL on 21/22. 3) Try address 0x3F in Settings → LCD (backpack address varies by batch). The machine itself keeps working regardless. |
| **Panel won't open** | Confirm the client is on `ESP32_4in1_Carwash`; browse to `http://192.168.4.1` directly. More than 4 clients connected? Raise `apMaxClients` (up to 8). |
| **Forgot PIN** | Hold BOOT 5 s → factory reset → reconfigure ([§19](#19-factory-reset-and-pin-recovery)). |
| **Credit disappeared** | A restart or power cut cleared the pool — credit is RAM-only by design ([§7](#7-everyday-operation-customer-flow)). Also check the time model: per-function drain burns credit faster with several functions running. |
| **Wrong amount credited** | A pulse group matching no preset is prorated from the nearest lower preset ([§10](#10-coin-and-bill-acceptor-setup)). Give that exact pulse count its own preset so every coin matches. |
| **Keeps rebooting** | Read "reset reason" in Diagnostics. Low-heap restarts → power supply problem. Brownout → the 5 V rail sags when relays switch; bigger supply or bulk capacitors ([§4](#4-power-and-grounding)). |
| **Function won't start** | 1) No credit (LCD shows OFF). 2) In allocated mode another function holds the credit — stop it first. 3) Run cap reached — stop the function first. |
| **Buttons dead** | Buttons wire GPIO→GND (not to 5 V!). Check the pin map in Settings → Buttons & Relays. |
| **Buzzer silent** | Check `buzzerEnabled`, the pin, and that the buzzer is passive (tone-driven), not an active self-oscillating one. |
| **"Save timed out" in the panel** | The controller did not confirm the new revision in time (it was busy or the network hiccupped). Check your values and press Save again; nothing is half-applied — configs apply atomically. |
| **Two customers, one machine** | Not supported — one shared credit pool per controller; a second bay needs a second controller. |

## 21. Developer Notes

- **Firmware identity:** `Carwash 4-in-1 Controller` v1.2.0, build tag 2026.11.21 (v2 source, 6,604 lines).
- **Libraries used:** `WiFi`, `WebServer`, `DNSServer`, `Preferences`, `Wire`, `LiquidCrystal_I2C`, `esp_timer` — all standard ESP32 Arduino core components; no third-party dependencies.
- **Storage layout (NVS namespace `cw4in1`):** `cfgA`/`cfgB` = alternating config slots (magic 0x34574331, version 3), `meta` = slot selector + write counter, `stat` = statistics blob (magic 0x53544154, version 3, journal pointers validated on load).
- **Task model:** web server + DNS run on core 0 (stack 12 KB, priority 1); the real-time control loop (coins, buttons, relays, LCD, buzzers) runs on the main task so web activity can never stall a wash in progress. Commands from the web flow through an 8-slot queue; the control loop takes only commands whose scheduled time has arrived, so a delayed command never blocks urgent ones.
- **Config pipeline:** web handler → `configPushIfCurrent` (one pending save at a time, guarded by revision) → control loop validates → saves to the alternate slot → applies live (pins re-attached, LCD re-init, AP deferred restart) → bumps `cfgRevision` → panel confirms by watching `/api/state`.
- **Web UI:** a single embedded HTML page (no CDN, no internet dependency), ~6 KB JSON buffer, 4 KB form limit, 1 s live polling with exponential backoff when offline.
- **Known harmless quirks in this build** (verified by code audit): a few unused constants and variables exist (`CONTROL_SLICE_US`, `RUNNING_WINDOW_US`, `RELAY_SETTLE_MS`, `SESSION_SAVE_MIN_US`, `cmdPop()`, `g_coinIsrCount/Seen`, `g_factoryConfirmed`, `g_cfgShadow`); the `restartRequired` flag is published in API JSON but never set to 1. None affect operation.
- **Web panel route map:** the UI uses hash routes (`#/dashboard`, `#/inventory`, `#/settings/general`, `#/settings/coin`, `#/settings/lcd`, `#/settings/io`, `#/settings/buzzer`, `#/settings/wifi`, `#/settings/system`).
