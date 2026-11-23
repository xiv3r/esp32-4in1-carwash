| Connection | GPIO | Notes |
|---|---|---|
| WATER relay | **17** | Relay module input IN1 |
| BLOWER relay | **18** | Relay module input IN2 |
| SOAP relay | **13** | Relay module input IN3 |
| FOAM relay | **14** | Relay module input IN4 |
| WATER button | 6 | Button connects GPIO to GND |
| BLOWER button | 7 | Button connects GPIO to GND |
| SOAP button | 15 | Button connects GPIO to GND |
| FOAM button | 16 | Button connects GPIO to GND |
| Coin/bill acceptor pulse | 4 | FALLING-edge pulse input, pull-up |
| Pulse output (mirror) | 5 | Optional copy of accepted pulses for external counters (60 ms wide, configurable 10–1000 ms) |
| Buzzer | 21 | `tone()` driven |
| LCD SDA | 8 | I2C data |
| LCD SCL | 9 | I2C clock |
| BOOT button (on the ESP32 board itself) | 0 | Hold 5 s for factory reset |
