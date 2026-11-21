## Important before you begin

This sketch has compiled and passed host-simulation tests, **but has not been tested on a physical machine**. Do not connect pumps, valves, a blower, or mains power until the control circuit has been checked with its loads disconnected. Have a qualified technician handle power wiring, isolation, fuses, emergency stop and electrical protection. Software is not an emergency stop.

ESP32 GPIO pins are **3.3 V only**. Many coin acceptors use 12 V, and a 5 V LCD backpack may pull its I²C lines up to 5 V. Use appropriate isolation or level shifting; never connect a 5 V or 12 V signal directly to a GPIO pin. Check relay inputs accept the ESP32's 3.3 V logic. Follow the specifications for your particular modules.

## 1. What you need

- A standard ESP32 DevKit and a USB data cable.
- A 20-column × 4-row I²C LCD, a four-channel relay module, four normally open push buttons, a pulse-output coin acceptor, and a buzzer.
- Correctly rated, regulated power supplies and protective components for your actual hardware. Many coin acceptors require 12 V; check its label.
- A computer with Arduino IDE and a phone or computer with Wi-Fi.

## 2. Default connections

These are **ESP32 GPIO numbers**, not physical pin positions. Disconnect power before wiring.

| Connection | GPIO |
|---|---:|
| WATER relay / button | 13 / 32 |
| BLOWER relay / button | 16 / 33 |
| SOAP relay / button | 17 / 27 |
| FOAM relay / button | 18 / 14 |
| Coin pulse input | 25 |
| Optional pulse output | 26 |
| Buzzer | 23 |
| LCD SDA / SCL | 21 / 22 |
| Factory reset | 0 — onboard BOOT button |

Buttons are normally open and connect their input to ground when pressed. The default relay setting is **active-low**: confirm this matches your module. The default LCD address is `0x27`; some backpacks use `0x3F`. Use an appropriate circuit and grounding scheme for the isolation you choose.

**Boot safety:** ESP32 pins can change state before the sketch starts or during reset/power loss. Verify the relay module remains off in all those situations; the sketch alone cannot guarantee this. Use suitable independent fail-safe hardware.

## 3. Upload the sketch

1. In Arduino IDE, install the Espressif **ESP32 board package**, version **4.0.0**. Install **LiquidCrystal I2C** version **1.1.2**. The remaining libraries come with the ESP32 board package.
2. Open `firmware/Carwash_4in1/Carwash_4in1.ino`. It is a single sketch; no extra code tabs are needed.
3. Select your ESP32 Dev Module and its USB port. Select the **Minimal SPIFFS** (`min_spiffs`) partition scheme, or another scheme with sufficient app space for your board.
4. Click **Verify**, then **Upload**. Follow your board's BOOT-button procedure if upload does not start.

The checked build used 1,178,923 bytes of app space. The library produced an AVR-architecture metadata warning, but compilation for ESP32 succeeded. The sketch does not use serial printing; an ESP32 ROM boot message may still appear before it runs.

## 4. Connect to the panel

1. Power the controller. Join Wi-Fi **ESP32_4in1_Carwash** with password **12345678**.
2. Open **http://192.168.4.1/** in a browser if the captive portal does not appear.
3. Under **Settings → Wi-Fi Access Point**, set a new network name and password. Use 8–63 printable characters for the password. Reconnect to the new network after saving.
4. Under **Settings → General**, set a numeric panel PIN and remember it. The default PIN is empty. The PIN protects commands and configuration/backup reads when set.

The panel uses **unencrypted HTTP**, and the PIN is not rate-limited. Use only on a trusted local network; do not expose it to the internet. Configuration backups include credentials, so keep them private.

## 5. Configure and test before connecting loads

1. With loads disconnected, check that every relay is off on power-up, reset, power loss and idle. Verify polarity and GPIO assignments in **Settings → Buttons & Relays**. Do not rely only on an indicator LED.
2. In **Settings → LCD 20x4**, verify the address, GPIO 21/22 and 20×4 layout. Check all four lines.
3. In **Settings → Coin & Bill Acceptor**, configure the actual acceptor's pulse count, denomination, pulse spacing and credit. Test each denomination several times and check pulse and sales counts. Defaults are:

   | Pulses | Credit | Recorded value |
   |---:|---:|---:|
   | 1 | 5 minutes | ₱1 |
   | 5 | 25 minutes | ₱5 |
   | 10 | 50 minutes | ₱10 |
   | 20 | 60 minutes | ₱20 |

   Pulses closer than 30 ms are filtered by default; a 350 ms quiet gap ends a group. Adjust to the acceptor. Do not assume these defaults match its programming.
4. Add test credit and check each button and relay separately, then check simultaneous operation, pause/resume, and relay shutdown when credit expires. Verify the LCD and panel agree.
5. Test the independent emergency stop and power-loss behavior. Only after a qualified technician confirms the installation is safe should the loads be connected and tested.

## 6. Daily operation

- Insert a configured coin/bill to add credit. By default, all functions share one pool, which drains at one second per second while any function runs.
- **Single press:** start a function, or resume it if paused.
- **Double press:** pause or resume that function.
- **Long press:** add time according to the configured pool and bonus settings; the default long-press threshold is 900 ms and the default addition is 60 seconds. Bonus time is enabled by default and has a configurable limit.
- Use the panel dashboard to view credit and control functions. Use **Sales Inventory** to review recorded totals.

Credit is kept in RAM and **is lost on reboot or power failure**. Statistics are saved periodically, so recent statistics can also be lost on power failure. The loop caps a single elapsed-time update at five seconds: a longer stall can undercount time. Test actual timing under real operating conditions.

## 7. Reset and troubleshooting

- **No LCD:** check power, 3.3 V-safe I²C wiring, SDA/SCL and address. Some backpacks use `0x3F`. Reconnect only when safe for your hardware.
- **No coin credit:** check the acceptor's power and isolated pulse output, GPIO 25, actual pulse timing and configured denominations. Test without machinery connected.
- **Cannot reach panel:** reconnect to its Wi-Fi and open `http://192.168.4.1/`. A changed network name or password requires reconnecting.
- **Forgot PIN:** once BOOT has been released after startup, hold it for the configured reset time (default **5 seconds**). This erases configuration and statistics and restarts with the default Wi-Fi credentials. Do this only when it is safe to stop the machine.
