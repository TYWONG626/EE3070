#include <Arduino.h>
#include <ESP32Servo.h>
#include "HX711.h"
#include <Wire.h>
#include "RTClib.h"
#include <WiFi.h>
#include "time.h"

// ================== Pins & devices ==================
const int LOADCELL_DOUT_PIN = 16; // DT
const int LOADCELL_SCK_PIN  = 4;  // SCK
const int foodPin  = 18;
const int waterPin = 17;

HX711 scale;
Servo foodServo;
Servo waterServo;
RTC_DS3231 rtc;

// ================== Calibration & filters ==================
float calibration_factor = 203.0f; // <<< adjust after calibration
float user_adjust_g      = 0.0f;   // <<< user correction offset (g)
const float DEADBAND     = 1.0f;   // <<< small readings treated as zero (g)

// ================== Timers ==================
unsigned long lastPrint = 0;       // periodic display timer (ms)

// ================== Wi-Fi + NTP settings ==================
const char* ssid     = "EE3070_P1615_1";  //"EE3070_P1615_1";
const char* password = "EE3070P1615";  //"EE3070P1615";
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 8 * 3600;   // UTC+8 for Hong Kong
const int   daylightOffset_sec = 0;

int foodAngle  = 0;
int waterAngle = 90;

// Calibration constants
float gramsPerSecond = 6.0;   // food dispensing rate (g/s)
float mlPerSecond    = 20.0;  // water dispensing rate (mL/s)

// Delay-based scheduling
unsigned long foodTriggerTime  = 0;
unsigned long foodReturnTime   = 0;
unsigned long waterTriggerTime = 0;
unsigned long waterReturnTime  = 0;

// RTC-based scheduling
bool   foodScheduled  = false;
bool   waterScheduled = false;
float  foodAmount     = 0;
float  waterAmount    = 0;
DateTime foodTargetDT;
DateTime waterTargetDT;

// Countdown helpers
long lastFoodCountdownSec  = -1;
long lastWaterCountdownSec = -1;

// ================== Setup ==================
void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 + HX711 + DS3231 Turtle Food Logger");
  foodServo.attach(foodPin);
  waterServo.attach(waterPin);
  foodServo.write(foodAngle);
  waterServo.write(waterAngle);

  // HX711 init
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(calibration_factor);

  Serial.println("Taring... remove any weight.");
  delay(2000);
  scale.tare();
  Serial.println("Tare done.");

  // RTC init (SDA=8, SCL=9)
  Wire.begin(8, 9);
  if (!rtc.begin()) {
    Serial.println("Couldn't find DS3231 RTC. Check wiring!");
    while (1);
  }

  // ===== Wi-Fi + NTP sync =====
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      DateTime now(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
      );
      rtc.adjust(now);  // Sync DS3231 with NTP
      Serial.println("RTC synchronized with NTP");
    } else {
      Serial.println("Failed to get time from NTP, using RTC value");
    }
  } else {
    Serial.println("\nWi-Fi connect failed, using RTC value");
  }

  if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting time to compile time...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  Serial.println("Commands:");
  Serial.println("  clear                  -> tare to 0");
  Serial.println("  cal <factor>           -> set calibration factor directly");
  Serial.println("  invert                 -> flip sign of calibration factor");
  Serial.println("  adjust <grams>         -> set correction offset in grams");
  Serial.println("  log <time>             -> e.g. log 10s or log 1m (duration)");
  Serial.println("  log HH:MM:SS-HH:MM:SS  -> log using RTC start/end");
  Serial.println("  settime HH:MM:SS       -> manually set RTC time for today");
  Serial.println();
  Serial.println("=== Smart Feeder Control (Food + Water) ===");
  Serial.println("Commands:");
  Serial.println("  f 3(20)        -> Feed after 3s, 20 g");
  Serial.println("  w 5(50)        -> Water after 5s, 50 mL");
  Serial.println("  f 12:00:00(20) -> Feed at 12:00:00, 20 g");
  Serial.println("  w 22:30:00(50) -> Water at 22:30:00, 50 mL");
  Serial.println("  t 2025/11/01 09:14:11 -> Set RTC date/time");
  Serial.println("===========================================");
}

// ================== Loop ==================
void loop() {
   DateTime now = rtc.now();
  // Handle serial commands
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length()) handleCommand(cmd);
  }

  // Periodic time-stamped weight display
  if (millis() - lastPrint >= 2000) {
    lastPrint = millis();

    if (scale.is_ready()) {
      scale.set_scale(calibration_factor);
      float grams = scale.get_units(20);
      if (fabs(grams) < DEADBAND) grams = 0.0f;
      float grams_corrected = grams + user_adjust_g;

      DateTime now = rtc.now();
      Serial.printf("[%02d:%02d:%02d] Current weight: %.2f g\n",
                    now.hour(), now.minute(), now.second(),
                    grams_corrected);
    } else {
      Serial.println("Scale not ready.");
    }
  }
    unsigned long nowMillis = millis();

  // Handle serial input
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    int start = 0;
    while (start < input.length()) {
      int sep = input.indexOf(';', start);
      String command = (sep == -1) ? input.substring(start) : input.substring(start, sep);
      start = (sep == -1) ? input.length() : sep + 1;

      command.trim();
      if (command.length() > 0) {
        processCommand(command);
      }
    }
  }

  // ===== Delay-based triggers =====
  if (foodTriggerTime > 0 && nowMillis >= foodTriggerTime) {
    foodAngle = 45;
    foodServo.write(foodAngle);
    Serial.println("Food servo opened (delay-based)");
    foodTriggerTime = 0;
  }
  if (foodReturnTime > 0 && nowMillis >= foodReturnTime) {
    foodAngle = 0;
    foodServo.write(foodAngle);
    Serial.println("Food servo closed");
    foodReturnTime = 0;
  }

  if (waterTriggerTime > 0 && nowMillis >= waterTriggerTime) {
    waterAngle = 0;
    waterServo.write(waterAngle);
    Serial.println("Water servo opened (delay-based)");
    waterTriggerTime = 0;
  }
  if (waterReturnTime > 0 && nowMillis >= waterReturnTime) {
    waterAngle = 90;
    waterServo.write(waterAngle);
    Serial.println("Water servo closed");
    waterReturnTime = 0;
  }

  // ===== RTC-based triggers =====
  now = rtc.now();

  if (foodScheduled) {
    TimeSpan remaining = foodTargetDT - now;
    long remainingSec = remaining.totalseconds();
    if (remainingSec < 0) remainingSec = 0;

    if (remainingSec > 0 && remainingSec != lastFoodCountdownSec) {
      Serial.printf("Food servo will activate in %ld seconds...\n", remainingSec);
      lastFoodCountdownSec = remainingSec;
    }

    if (!(now < foodTargetDT)) {
      foodAngle = 45;
      foodServo.write(foodAngle);
      Serial.printf("Food servo activated at %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());

      unsigned long durationMs = (unsigned long)((foodAmount / gramsPerSecond) * 1000.0);
      foodReturnTime = millis() + durationMs;
      foodScheduled = false;
      lastFoodCountdownSec = -1;
    }
  }

  if (waterScheduled) {
    TimeSpan remaining = waterTargetDT - now;
    long remainingSec = remaining.totalseconds();
    if (remainingSec < 0) remainingSec = 0;

    if (remainingSec > 0 && remainingSec != lastWaterCountdownSec) {
      Serial.printf("Water servo will activate in %ld seconds...\n", remainingSec);
      lastWaterCountdownSec = remainingSec;
    }

    if (!(now < waterTargetDT)) {
      waterAngle = 0;
      waterServo.write(waterAngle);
      Serial.printf("Water servo activated at %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());

      unsigned long durationMs = (unsigned long)((waterAmount / mlPerSecond) * 1000.0);
      waterReturnTime = millis() + durationMs;
      waterScheduled = false;
      lastWaterCountdownSec = -1;
    }
  }
}

// ================== Command handling ==================
void handleCommand(const String& cmd) {
  // Tare
  if (cmd.equalsIgnoreCase("clear")) {
    Serial.println("Taring... remove any weight.");
    delay(800);
    scale.tare();
    Serial.println("Tare complete.");
    return;
  }

  // Adjust offset
  if (cmd.startsWith("adjust")) {
    String arg = cmd.substring(6); arg.trim();
    user_adjust_g = arg.toFloat();
    Serial.printf("Adjust offset set to %.2f g\n", user_adjust_g);
    return;
  }

  // Calibration factor
  if (cmd.startsWith("cal")) {
    String arg = cmd.substring(3); arg.trim();
    calibration_factor = arg.toFloat();
    scale.set_scale(calibration_factor);
    Serial.printf("Calibration factor set to %.6f\n", calibration_factor);
    return;
  }

  // Invert calibration factor
  if (cmd.equalsIgnoreCase("invert")) {
    calibration_factor = -calibration_factor;
    scale.set_scale(calibration_factor);
    Serial.printf("Calibration factor inverted. New factor: %.6f\n", calibration_factor);
    return;
  }

  // Manually set RTC time for today (HH:MM:SS)
  if (cmd.startsWith("settime")) {
    String arg = cmd.substring(7); arg.trim();
    int hh, mm, ss;
    if (sscanf(arg.c_str(), "%d:%d:%d", &hh, &mm, &ss) == 3) {
      DateTime cur = rtc.now();
      DateTime newTime(cur.year(), cur.month(), cur.day(), hh, mm, ss);
      rtc.adjust(newTime);
      Serial.printf("RTC time set to %02d:%02d:%02d\n", hh, mm, ss);
    } else {
      Serial.println("Invalid format. Use: settime HH:MM:SS");
    }
    return;
  }

  // Logging commands
  if (cmd.startsWith("log")) {
    String arg = cmd.substring(3); arg.trim();
    if (!arg.length()) {
      Serial.println("Usage: log <time>  e.g., log 10s or log 1m");
      return;
    }

    // RTC window: HH:MM:SS-HH:MM:SS
    if (arg.indexOf('-') != -1) {
      int sh, sm, ss, eh, em, es;
      if (sscanf(arg.c_str(), "%d:%d:%d-%d:%d:%d", &sh, &sm, &ss, &eh, &em, &es) == 6) {
        DateTime now = rtc.now();
        DateTime start(now.year(), now.month(), now.day(), sh, sm, ss);
        DateTime end(now.year(), now.month(), now.day(), eh, em, es);
        if (end < start) {
          end = DateTime(now.year(), now.month(), now.day()+1, eh, em, es);
        }
        logFoodRTC(start, end);
      } else {
        Serial.println("Invalid RTC log format. Use: log HH:MM:SS-HH:MM:SS");
      }
    } else {
      // Duration: e.g., 10s or 1m
      unsigned long waitMs = parseDuration(arg);
      if (waitMs == 0) {
        Serial.println("Invalid time format. Use e.g. 10s or 1m");
        return;
      }
      logFood(waitMs);
    }
    return;
  }

  Serial.println("Unknown command. Use: clear | adjust <grams> | cal <factor> | invert | log <time> | log HH:MM:SS-HH:MM:SS | settime HH:MM:SS");
}

// Parse duration string like "10s" or "1m" into milliseconds
unsigned long parseDuration(const String& s) {
  if (s.endsWith("s")) {
    int val = s.substring(0, s.length()-1).toInt();
    return (unsigned long)val * 1000UL;
  }
  if (s.endsWith("m")) {
    int val = s.substring(0, s.length()-1).toInt();
    return (unsigned long)val * 60000UL;
  }
  return 0;
}

// Perform before/after logging by duration (blocking delay)
void logFood(unsigned long waitMs) {
  DateTime now = rtc.now();
  Serial.printf("[%02d:%02d:%02d] Starting food log (duration %lus)...\n",
                now.hour(), now.minute(), now.second(), waitMs/1000);

  // Before weight
  float before = scale.get_units(20);
  if (fabs(before) < DEADBAND) before = 0.0f;
  before += user_adjust_g;
  now = rtc.now();
  Serial.printf("[%02d:%02d:%02d] Before weight: %.2f g\n",
                now.hour(), now.minute(), now.second(), before);

  // Wait
  delay(waitMs);

  // After weight
  float after = scale.get_units(20);
  if (fabs(after) < DEADBAND) after = 0.0f;
  after += user_adjust_g;
  now = rtc.now();
  Serial.printf("[%02d:%02d:%02d] After weight: %.2f g\n",
                now.hour(), now.minute(), now.second(), after);

  // Difference
  float eaten = before - after;
  if (eaten < 0) eaten = 0.0f;
  now = rtc.now();
  Serial.printf("[%02d:%02d:%02d] Turtle has eaten %.2f g of food.\n",
                now.hour(), now.minute(), now.second(), eaten);
}

// Perform logging within an RTC time window (blocking waits)
void logFoodRTC(DateTime start, DateTime end) {
  Serial.printf("RTC log scheduled from %02d:%02d:%02d to %02d:%02d:%02d\n",
                start.hour(), start.minute(), start.second(),
                end.hour(), end.minute(), end.second());

  // Wait until start
  while (rtc.now() < start) {
    delay(500);
  }

  // Before weight
  float before = scale.get_units(20);
  if (fabs(before) < DEADBAND) before = 0.0f;
  before += user_adjust_g;
  DateTime now = rtc.now();
  Serial.printf("[%02d:%02d:%02d] Weight at start: %.2f g\n",
                now.hour(), now.minute(), now.second(), before);

  // Wait until end
  while (rtc.now() < end) {
    delay(500);
  }

  // After weight
  float after = scale.get_units(20);
  if (fabs(after) < DEADBAND) after = 0.0f;
  after += user_adjust_g;
  now = rtc.now();
  Serial.printf("[%02d:%02d:%02d] Weight at end: %.2f g\n",
                now.hour(), now.minute(), now.second(), after);

  // Difference
  float eaten = before - after;
  if (eaten < 0) eaten = 0.0f;
  now = rtc.now();
  Serial.printf("[%02d:%02d:%02d] Turtle ate %.2f g between times.\n",
                now.hour(), now.minute(), now.second(), eaten);
}

void processCommand(String cmd) {
  cmd.trim();
  Serial.print("Received command: '");
  Serial.print(cmd);
  Serial.println("'");
  if (cmd.length() < 2) return;

  char type = cmd.charAt(0);

  // ===== Set RTC Date/Time =====
  if (type == 't' || type == 'T') {
    int yy, MM, dd, hh, mm, ss;
    if (sscanf(cmd.c_str() + 2, "%d/%d/%d %d:%d:%d", &yy, &MM, &dd, &hh, &mm, &ss) == 6) {
      DateTime newTime(yy, MM, dd, hh, mm, ss);
      rtc.adjust(newTime);
      Serial.printf("RTC date/time manually set to %04d/%02d/%02d %02d:%02d:%02d\n",
                    yy, MM, dd, hh, mm, ss);
    } else {
      Serial.println("Invalid format. Use: t YYYY/MM/DD HH:MM:SS");
    }
    return;
  }

  // ===== Parse feed/water commands =====
  int spaceIndex  = cmd.indexOf(' ');
  int parenStart  = cmd.indexOf('(');
  int parenEnd    = cmd.indexOf(')');

  if (spaceIndex == -1 || parenStart == -1 || parenEnd == -1) {
    Serial.println("Invalid format. Use f 3(20) or w 12:00:00(50)");
    return;
  }

  String timeOrDelay = cmd.substring(spaceIndex + 1, parenStart);
  String amountStr   = cmd.substring(parenStart + 1, parenEnd);
  float amount = amountStr.toFloat();
  if (amount <= 0) {
    Serial.println("Invalid amount.");
    return;
  }

  bool isFood = (type == 'f' || type == 'F');
  bool isWater = (type == 'w' || type == 'W');

  if (!isFood && !isWater) {
    Serial.println("Unknown command type. Use 'f' or 'w'.");
    return;
  }

  // ===== Absolute time =====
  if (timeOrDelay.indexOf(':') != -1) {
    int hh, mm, ss;
    if (sscanf(timeOrDelay.c_str(), "%d:%d:%d", &hh, &mm, &ss) != 3) {
      Serial.println("Invalid time format. Use HH:MM:SS");
      return;
    }

    DateTime now = rtc.now();
    DateTime candidate(now.year(), now.month(), now.day(), hh, mm, ss);
    if (candidate.unixtime() <= now.unixtime()) {
      candidate = DateTime(now.year(), now.month(), now.day() + 1, hh, mm, ss);
    }

    if (isFood) {
      foodTargetDT = candidate;
      foodAmount = amount;
      foodScheduled = true;
      lastFoodCountdownSec = -1;
      Serial.printf("Food servo scheduled at %02d:%02d:%02d for %.2f g.\n", hh, mm, ss, amount);
    } else {
      waterTargetDT = candidate;
      waterAmount = amount;
      waterScheduled = true;
      lastWaterCountdownSec = -1;
      Serial.printf("Water servo scheduled at %02d:%02d:%02d for %.2f mL.\n", hh, mm, ss, amount);
    }
  }
  // ===== Delay in seconds =====
  else {
    int delaySeconds = timeOrDelay.toInt();
    if (delaySeconds <= 0) {
      Serial.println("Invalid delay value.");
      return;
    }

    unsigned long nowMillis  = millis();
    unsigned long triggerTime = nowMillis + (unsigned long)delaySeconds * 1000UL;
    unsigned long durationMs;

    if (isFood) {
      durationMs = (unsigned long)((amount / gramsPerSecond) * 1000.0);
      foodTriggerTime = triggerTime;
      foodReturnTime  = triggerTime + durationMs;
      Serial.printf("Food servo scheduled in %d s to dispense %.2f g.\n", delaySeconds, amount);
    } else {
      durationMs = (unsigned long)((amount / mlPerSecond) * 1000.0);
      waterTriggerTime = triggerTime;
      waterReturnTime  = triggerTime + durationMs;
      Serial.printf("Water servo scheduled in %d s to dispense %.2f mL.\n", delaySeconds, amount);
    }
  }
}
