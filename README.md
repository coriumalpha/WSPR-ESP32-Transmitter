# ESP32 + Si5351 WSPR Transmitter

This Arduino sketch generates a precise WSPR beacon using an ESP32 and an Etherkit Si5351A clock generator.

### Features
- Accurate NTP-based timing (no RTC required)
- Native support for ESP32 core 3.x timers
- Optional 30 s oscillator warm-up
- Tested on 20 m band (14.0971 MHz RF)

### Hardware
- ESP32 DevKit V1 (30-pin)
- Si5351A module (I²C pins: SDA = 21, SCL = 22)
- Low-pass filter for RF output

### Configuration
Edit in the source:
```cpp
const char* ssid = "YOUR_WIFI";
const char* pass = "YOUR_PASS";
char call[7] = "CALLSIGN";
char loc[5]  = "GRID";
#define DIAL_20M_HZ   14097100UL
#define CORRECTION_HZ  <your_offset_in_Hz>
