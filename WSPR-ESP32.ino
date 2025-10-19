/*
  ESP32 + Si5351 (Etherkit) WSPR Transmitter – 20 m band
  -------------------------------------------------------
  Generates a WSPR beacon directly on RF using the Etherkit Si5351 library.

  Features:
  - Native NTP time sync (no external RTC required)
  - Precise 0.682687 s WSPR symbol timing using ESP32 hardware timer
  - Optional 30 s warm-up for frequency stabilization
  - Supports fine frequency correction in Hz

  Author: EA2FGS - @coriumalpha
  License: MIT
*/

#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <JTEncode.h>
#include <si5351.h>      // Etherkit Si5351 library
#include <TimeLib.h>     // For minute(), second(), etc.

// -------------------- User Configuration --------------------
const char* ssid = "YOUR_WIFI_SSID";
const char* pass = "YOUR_WIFI_PASSWORD";

char   call[7] = "CALLSIGN";   // Your callsign (e.g., "EA2FGS")
char   loc[5]  = "GRID";       // Maidenhead locator (e.g., "IN83")
uint8_t dbm    = 10;           // Transmit power (dBm) reported in WSPR payload

// WSPR 20 m band: Dial frequency (USB mode)
#define DIAL_20M_HZ     14097100UL     // 14.097100 MHz RF center
#define CORRECTION_HZ   -2392          // Fine frequency correction (measured offset in Hz)

// -------------------- WSPR Constants --------------------
#define SYMBOL_COUNT      WSPR_SYMBOL_COUNT
#define TONE_SPACING_CHZ  146          // 1.46 Hz tone spacing = 146 centi-Hz
#define SYM_US            682687UL     // WSPR symbol duration = 0.682687 seconds

// -------------------- Globals --------------------
Si5351 si5351;
JTEncode jt;
uint8_t txbuf[SYMBOL_COUNT];

hw_timer_t* tmr = nullptr;
volatile bool tick = false;
bool warmed_up = false;  // Tracks whether the Si5351 is pre-heated

// -------------------- Timer ISR --------------------
void IRAM_ATTR onTick() { tick = true; }

// -------------------- Time Setup (NTP) --------------------
void setupTime() {
  // Time zone: Central Europe (with daylight saving)
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();

  // Sync time from NTP servers
  configTime(0, 0, "pool.ntp.org", "time.google.com", "europe.pool.ntp.org");

  // Wait until time is valid (epoch > 8h)
  for (int i = 0; i < 50 && time(nullptr) < 8 * 3600; i++) delay(200);

  // Sync TimeLib with system time
  setSyncProvider([]() { return time(nullptr); });
  setSyncInterval(300);
}

// -------------------- Setup --------------------
void setup() {
  Serial.begin(115200);
  delay(50);

  // --- WiFi connection ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
  }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());

  setupTime();

  // --- I2C initialization ---
  Wire.begin(21, 22);           // SDA=21, SCL=22 (ESP32 DevKit)
  Wire.setClock(400000);

  // --- Si5351 setup ---
  if (!si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0)) {
    Serial.println("Si5351 init FAILED!");
    while (true) delay(1000);
  }

  // Optional: Si5351 crystal correction (in ppb)
  // si5351.set_correction(ppb_value, SI5351_PLL_INPUT_XO);

  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA);
  si5351.set_clock_pwr(SI5351_CLK0, 0); // Start with output off

  // --- Hardware timer ---
  tmr = timerBegin(1000000);             // 1 MHz base (ticks in microseconds)
  timerAttachInterrupt(tmr, &onTick);
  timerAlarm(tmr, SYM_US, true, 0);      // 0.682687 s periodic interrupt

  Serial.println("Setup complete.");
}

// -------------------- Helpers --------------------
uint64_t base_chz() {
  uint64_t hz = (uint64_t)DIAL_20M_HZ + (int32_t)CORRECTION_HZ;
  return hz * 100ULL; // Convert to centi-Hz for Si5351 library
}

void start_warmup() {
  // Enable Si5351 output 30 s before the TX slot
  si5351.set_freq(base_chz(), SI5351_CLK0);
  si5351.set_clock_pwr(SI5351_CLK0, 1);
  warmed_up = true;
  Serial.println("Warm-up started (30 s before TX)");
}

void stop_output() {
  si5351.set_clock_pwr(SI5351_CLK0, 0);
}

// -------------------- WSPR Transmission --------------------
void wspr_tx() {
  jt.wspr_encode(call, loc, dbm, txbuf);

  if (!warmed_up) {
    si5351.set_freq(base_chz(), SI5351_CLK0);
    si5351.set_clock_pwr(SI5351_CLK0, 1);
  }

  Serial.println("TX START");
  uint32_t t0 = millis();

  for (uint16_t i = 0; i < SYMBOL_COUNT; i++) {
    uint64_t f_chz = base_chz() + (uint64_t)txbuf[i] * TONE_SPACING_CHZ;
    si5351.set_freq(f_chz, SI5351_CLK0);
    tick = false;
    while (!tick) { /* wait for next 0.682687 s symbol */ }
  }

  stop_output();
  warmed_up = false;

  uint32_t dt = millis() - t0;
  Serial.printf("TX END (duration %lu ms)\n", (unsigned long)dt);
}

// -------------------- Main Loop --------------------
void loop() {
  if (timeStatus() != timeSet) { delay(200); return; }

  // Start warm-up exactly 30 s before an even minute slot
  if (!warmed_up && (minute() % 2 == 1) && (second() == 30)) {
    start_warmup();
  }

  // Start WSPR transmission exactly on even minute, second 00
  if ((minute() % 2 == 0) && (second() == 0)) {
    delay(5); // debounce
    if (second() <= 1) wspr_tx();
  }

  delay(20);
}
