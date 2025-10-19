/*
  ESP32 + Si5351 (Etherkit) WSPR 20 m – NTP-synced, RF-muted warm-up
  -------------------------------------------------------------------
  - Direct RF WSPR beacon on 14.0971 MHz (20 m band)
  - Exact WSPR timing via ESP32 hardware timer (core 3.x)
  - NTP time sync (no RTC)
  - 30 s warm-up with PLL active but RF muted (no on-air interference)
  - Fine frequency correction in Hz

  Wiring (ESP32 DevKit V1 30-pin):
    I2C SDA -> GPIO21
    I2C SCL -> GPIO22
    Si5351 default I2C addr: 0x60

  Author: EA2FGS - @coriumalpha
  License: MIT
*/

#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <JTEncode.h>
#include <si5351.h>      // Etherkit Si5351
#include <TimeLib.h>     // minute(), second()

// -------------------- User Configuration --------------------
const char* ssid = "YOUR_WIFI_SSID";
const char* pass = "YOUR_WIFI_PASSWORD";

char   call[7] = "CALLSIGN";   // e.g. "EA0RGS"
char   loc[5]  = "GRID";       // e.g. "IM73"
uint8_t dbm    = 10;           // Reported power (dBm) in WSPR payload

// 20 m WSPR RF center
#define DIAL_20M_HZ     14097100UL   // 14.097100 MHz RF
#define CORRECTION_HZ   (-2300)      // set from your calibration (+2.300 kHz high → -2300)

// -------------------- WSPR Constants --------------------
#define SYMBOL_COUNT      WSPR_SYMBOL_COUNT
#define TONE_SPACING_CHZ  146        // ≈1.46 Hz = 146 centi-Hz
#define SYM_US            682687UL   // 0.682687 s/symbol

// -------------------- Globals --------------------
Si5351 si5351;
JTEncode jt;
uint8_t txbuf[SYMBOL_COUNT];

hw_timer_t* tmr = nullptr;
volatile bool tick = false;
bool warmed_up = false;  // PLL prepared, RF muted

// -------------------- Timer ISR --------------------
void IRAM_ATTR onTick() { tick = true; }

// -------------------- NTP / Time --------------------
void setupTime() {
  // Central Europe with DST (adjust TZ if needed)
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.google.com", "europe.pool.ntp.org");

  // Wait until time is valid (epoch > 8h)
  for (int i = 0; i < 50 && time(nullptr) < 8 * 3600; i++) delay(200);

  // Tie TimeLib to system time
  setSyncProvider([]() { return time(nullptr); });
  setSyncInterval(300);
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);
  delay(50);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) { delay(200); Serial.print("."); }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());

  setupTime();

  // I2C
  Wire.begin(21, 22);     // SDA, SCL
  Wire.setClock(400000);

  // Si5351
  if (!si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0)) {
    Serial.println("Si5351 init FAIL");
    while (1) delay(1000);
  }
  // Optional XO correction in ppb (if known):
  // si5351.set_correction(ppb, SI5351_PLL_INPUT_XO);

  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA);
  si5351.set_clock_pwr(SI5351_CLK0, 0); // RF off by default

  // Timer @1 MHz (µs)
  tmr = timerBegin(1000000);
  timerAttachInterrupt(tmr, &onTick);
  timerAlarm(tmr, SYM_US, true, 0);

  Serial.println("Setup complete.");
}

// -------------------- Helpers --------------------
uint64_t base_chz() {
  // RF base in centi-Hz (Etherkit expects cHz)
  uint64_t hz = (uint64_t)DIAL_20M_HZ + (int32_t)CORRECTION_HZ;
  return hz * 100ULL;
}

void start_warmup() {
  // Prepare PLL at target frequency but keep RF muted to avoid on-air interference
  si5351.set_freq(base_chz(), SI5351_CLK0);
  si5351.set_clock_pwr(SI5351_CLK0, 0);  // RF OFF, PLL active internally
  warmed_up = true;
  Serial.printf("Warm-up (PLL ON, RF muted) @ %.6f MHz\n",
                (double)(DIAL_20M_HZ + (int32_t)CORRECTION_HZ)/1e6);
}

void stop_output() {
  si5351.set_clock_pwr(SI5351_CLK0, 0);
}

// -------------------- WSPR TX --------------------
void wspr_tx() {
  // Encode payload
  jt.wspr_encode(call, loc, dbm, txbuf);

  // Ensure RF is ON at start of TX (even if we warmed up muted)
  si5351.set_freq(base_chz(), SI5351_CLK0);
  si5351.set_clock_pwr(SI5351_CLK0, 1);

  Serial.println("TX ON");
  uint32_t t0 = millis();

  for (uint16_t i = 0; i < SYMBOL_COUNT; i++) {
    uint64_t f_chz = base_chz() + (uint64_t)txbuf[i] * TONE_SPACING_CHZ;
    si5351.set_freq(f_chz, SI5351_CLK0);
    tick = false;
    while (!tick) { /* wait 0.682687 s */ }
  }

  stop_output();
  warmed_up = false;
  uint32_t dt = millis() - t0;
  Serial.printf("TX OFF  (duration %lu ms)\n", (unsigned long)dt);
}

// -------------------- Main Loop --------------------
void loop() {
  if (timeStatus() != timeSet) { delay(200); return; }

  // Warm-up 30 s before even-minute slot (…:00, …:02, …:04…)
  // Minute odd and second 30 ==> 30 s before next even minute
  if (!warmed_up && (minute() % 2 == 1) && (second() == 30)) {
    start_warmup();
  }

  // Transmit exactly at even minute, second 00
  if ((minute() % 2 == 0) && (second() == 0)) {
    delay(5);                  // debounce re-entry
    if (second() <= 1) wspr_tx();
  }

  delay(20);
}
