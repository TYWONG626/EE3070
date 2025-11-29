#include <Arduino.h>
#include <ESP32Servo.h>
#include "HX711.h"
#include <Wire.h>
#include "RTClib.h"
#include <WiFi.h>
#include "time.h"
#include <HTTPClient.h>

// ======================================================
// 1. Pin Assignments
// ======================================================
// SDA=8, SCL=9
const int LOADCELL_DOUT_PIN = 16;   // HX711 DT
const int LOADCELL_SCK_PIN  = 4;    // HX711 SCK
const int FOOD_SERVO_PIN    = 18;   // Food servo
const int WATER_SERVO_PIN   = 17;   // Water servo

// ======================================================
// 2. Device Objects
// ======================================================
HX711 scale;
Servo foodServo;
Servo waterServo;
RTC_DS3231 rtc;

// ======================================================
// 3. Servo Angles (default positions)
// ======================================================
int foodAngle  = 90;
int waterAngle = 180;

// ======================================================
// 4. Calibration & Filters
// ======================================================
float calibration_factor = 203.0f;   // Adjust after calibration
float user_adjust_g      = 0.0f;     // User correction offset (g)
const float DEADBAND     = 1.0f;     // Small readings treated as zero (g)
float deliveredAmount    = 0.0f;

// Dispense rates
float gramsPerSecond = 6.0f;         // Food dispensing rate (g/s)
float mlPerSecond    = 20.0f;        // Water dispensing rate (mL/s)

// ======================================================
// 5. Stock Tracking
// ======================================================
float totalFoodStock   = 1000.0f;    // Starting food stock (g)
float totalWaterStock  = 1000.0f;    // Starting water stock (mL)
float foodDispensed    = 0.0f;
float waterDispensed   = 0.0f;
float foodDispenseAmount  = 0.0f;
float waterDispenseAmount = 0.0f;

// ======================================================
// 6. Timers & Scheduling
// ======================================================
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
float  remainingFood  = 0;
float  remainingWater = 0;
float  grams_corrected = 0;

DateTime foodTargetDT;
DateTime waterTargetDT;

// Countdown helpers
long lastFoodCountdownSec  = -1;
long lastWaterCountdownSec = -1;

// HX711 RTC-based scheduling
bool hx711Scheduled = false;
DateTime hx711StartDT;
DateTime hx711EndDT;
long lastHX711CountdownSec = -1;

// ======================================================
// 7. Wi-Fi + NTP Settings
// ======================================================
const char* ssid     = "EE3070_P1615_1";       // Wi-Fi SSID  //EE3070_P1615_1 
const char* password = "EE3070P1615";     // Wi-Fi Password  //EE3070P1615
const char* ntpServer = "pool.ntp.org";    // NTP server
const long  gmtOffset_sec     = 8 * 3600;  // UTC+8 for Hong Kong
const int   daylightOffset_sec = 0;

// ======================================================
// 8. ThingSpeak Settings
// ======================================================
const char* writeApiKey  = "LRT1SV5NLOOBPDKV";   // Channel Write API Key
const char* talkbackKey  = "U5DD6VCEZO0CIZEZ";   // TalkBack API Key
const unsigned long pollMs = 15000; // >=15s per ThingSpeak limits
unsigned long lastPoll = 0;
const char* tsUpdateURL  = "https://api.thingspeak.com/update";


// ================== Setup ==================
void setup() {
  // ======================================================
  // 1. Serial Monitor
  // ======================================================
  Serial.begin(115200);
  Serial.println("ESP32 + HX711 + DS3231 Turtle Food Logger");

  // ======================================================
  // 2. Servo Initialization
  // ======================================================
  foodServo.attach(FOOD_SERVO_PIN);
  waterServo.attach(WATER_SERVO_PIN);
  foodServo.write(foodAngle);   // default closed
  waterServo.write(waterAngle); // default closed

  // ======================================================
  // 3. HX711 Initialization
  // ======================================================
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(calibration_factor);
  Serial.println("Taring... remove any weight.");
  delay(2000);
  scale.tare();
  Serial.println("Tare done.");

  // ======================================================
  // 4. RTC Initialization
  // ======================================================
  Wire.begin(8, 9);  // SDA=8, SCL=9
  if (!rtc.begin()) {
    Serial.println("Couldn't find DS3231 RTC. Check wiring!");
    while (1); // halt if RTC not found
  }

  if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting time to compile time...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // ======================================================
  // 5. Wi-Fi + NTP Synchronization
  // ======================================================
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

  // ======================================================
  // 6. User Command Help
  // ======================================================
  Serial.println("=== Commands ===");
  Serial.println("  clear                  -> tare to 0");
  Serial.println("  cal <factor>           -> set calibration factor directly");
  Serial.println("  invert                 -> flip sign of calibration factor");
  Serial.println("  adjust <grams>         -> set correction offset in grams");
  Serial.println("  log <time>             -> e.g. log 10s or log 1m (duration)");
  Serial.println("  log HH:MM:SS-HH:MM:SS  -> log using RTC start/end");
  Serial.println("  settime HH:MM:SS       -> manually set RTC time for today");
  Serial.println("=== Smart Feeder Control (Food + Water) ===");
  Serial.println("  f 3(20)        -> Feed after 3s, 20 g");
  Serial.println("  w 5(50)        -> Water after 5s, 50 mL");
  Serial.println("  f 12:00:00(20) -> Feed at 12:00:00, 20 g");
  Serial.println("  w 22:30:00(50) -> Water at 22:30:00, 50 mL");
  Serial.println("  t YYYY/MM/DD HH:MM:SS -> Set RTC date/time");
  Serial.println("===========================================");
}


// ================== Loop ==================
void loop() {
  DateTime now = rtc.now();
  unsigned long nowMillis = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (nowMillis - lastPoll >= pollMs) {
      lastPoll = nowMillis;
      fetchAndExecuteTalkBack();
    }
  } else {
    WiFi.reconnect();
  }

  // ======================================================
  // 1. HX711 Refresh + Live Display
  // ======================================================
  if (scale.is_ready()) {
    float grams = scale.get_units(10);
    if (fabs(grams) < DEADBAND) grams = 0.0;
    grams_corrected = grams + user_adjust_g;   // live reading
  }

  Serial.printf("RTC time: %04d/%02d/%02d %02d:%02d:%02d | Live reading: %.2f g\n",
                now.year(), now.month(), now.day(),
                now.hour(), now.minute(), now.second(),
                grams_corrected);

  delay(1000);

  // ======================================================
  // 2. Handle Serial Input
  // ======================================================
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
        if (command.startsWith("clear") || command.startsWith("adjust") ||
            command.startsWith("cal")   || command.startsWith("invert") ||
            command.startsWith("log")   || command.startsWith("settime")) {
          handleCommand(command);
        } else {
          processCommand(command);
        }
      }
    }
  }

  // ======================================================
  // 3. Delay-Based Food Triggers
  // ======================================================
  if (foodTriggerTime > 90 && nowMillis >= foodTriggerTime) {
    foodServo.write(0);//Control servo angle
    Serial.println("Food servo opened (delay-based)");
    foodTriggerTime = 0;
  }

  if (foodReturnTime > 90 && nowMillis >= foodReturnTime) {
    foodServo.write(90);
    Serial.println("Food servo closed");
    foodReturnTime = 0;

    // Update food inventory
    if (foodDispenseAmount > 0.0) {
      foodDispensed += foodDispenseAmount;
      foodDispenseAmount = 0.0;
    } else if (foodAmount > 0.0) {
      foodDispensed += foodAmount;
      foodAmount = 0.0;
    }

    float remainingFood = totalFoodStock - foodDispensed;
    float percentFood = (remainingFood / totalFoodStock) * 100.0;
    Serial.printf("Remaining food: %.2f g (%.2f%%)\n", remainingFood, percentFood);

    // Refresh HX711 before sending
    if (scale.is_ready()) {
      float grams = scale.get_units(10);
      if (fabs(grams) < DEADBAND) grams = 0.0;
      grams_corrected = grams + user_adjust_g;
    }

    // Push to ThingSpeak
    sendToThingSpeak(remainingFood,
                     totalWaterStock - waterDispensed,
                     grams_corrected);
  }

  // ======================================================
  // 4. Delay-Based Water Triggers
  // ======================================================
  if (waterTriggerTime > 0 && nowMillis >= waterTriggerTime) {
    waterAngle = 0;//Control servo angle
    waterServo.write(waterAngle);
    Serial.println("Water servo opened (delay-based)");
    waterTriggerTime = 0;
  }

  if (waterReturnTime > 0 && nowMillis >= waterReturnTime) {
    waterAngle = 180;
    waterServo.write(waterAngle);
    Serial.println("Water servo closed");
    waterReturnTime = 0;

    // Update water inventory
    if (waterDispenseAmount > 0.0) {
      waterDispensed += waterDispenseAmount;
      waterDispenseAmount = 0.0;
    } else if (waterAmount > 0.0) {
      waterDispensed += waterAmount;
      waterAmount = 0.0;
    }

    float remainingWater = totalWaterStock - waterDispensed;
    float percentWater = (remainingWater / totalWaterStock) * 100.0;
    Serial.printf("Remaining water: %.2f mL (%.2f%%)\n", remainingWater, percentWater);

    // Refresh HX711 before sending
    if (scale.is_ready()) {
      float grams = scale.get_units(10);
      if (fabs(grams) < DEADBAND) grams = 0.0;
      grams_corrected = grams + user_adjust_g;
    }

    // Push to ThingSpeak
    sendToThingSpeak(totalFoodStock - foodDispensed,
                     remainingWater,
                     grams_corrected);
  }

  // ======================================================
  // 5. RTC-Based Food Scheduling
  // ======================================================
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
      foodAngle = 0;
      foodServo.write(foodAngle);
      Serial.printf("Food servo activated at %02d:%02d:%02d\n",
                    now.hour(), now.minute(), now.second());

      unsigned long durationMs = (unsigned long)((foodAmount / gramsPerSecond) * 1000.0);
      foodReturnTime = millis() + durationMs;
      foodScheduled = false;
      lastFoodCountdownSec = -1;
    }
  }

  // ======================================================
  // 6. RTC-Based Water Scheduling
  // ======================================================
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
      Serial.printf("Water servo activated at %02d:%02d:%02d\n",
                    now.hour(), now.minute(), now.second());

      unsigned long durationMs = (unsigned long)((waterAmount / mlPerSecond) * 1000.0);
      waterReturnTime = millis() + durationMs;
      waterScheduled = false;
      lastWaterCountdownSec = -1;
    }
  }

  // ======================================================
  // 7. HX711 RTC-based logging
  // ======================================================
  if (hx711Scheduled) {
    DateTime now = rtc.now();
    TimeSpan remaining = hx711StartDT - now;
    long remainingSec = remaining.totalseconds();
    if (remainingSec < 0) remainingSec = 0;

    // Countdown message
    if (remainingSec > 0 && remainingSec != lastHX711CountdownSec) {
      Serial.printf("HX711 log will start in %ld seconds...\n", remainingSec);
      lastHX711CountdownSec = remainingSec;
    }

    // Start logging when time arrives
    if (!(now < hx711StartDT)) {
      Serial.printf("HX711 log started at %02d:%02d:%02d\n",
                    now.hour(), now.minute(), now.second());

      // Before weight
      float before = scale.get_units(20);
      if (fabs(before) < DEADBAND) before = 0.0f;
      before += user_adjust_g;
      Serial.printf("[%02d:%02d:%02d] Weight at start: %.2f g\n",
                    now.hour(), now.minute(), now.second(), before);

      // Wait until end
      while (rtc.now() < hx711EndDT) {
        delay(500);
      }

      // After weight
      float after = scale.get_units(20);
      if (fabs(after) < DEADBAND) after = 0.0f;
      after += user_adjust_g;
      now = rtc.now();
      Serial.printf("[%02d:%02d:%02d] Weight at end: %.2f g\n",
                    now.hour(), now.minute(), now.second(), after);

      // Difference = amount eaten
      float grams_eaten = before - after;
      if (grams_eaten < 0) grams_eaten = 0.0f;
      grams_corrected = grams_eaten;

      Serial.printf("[%02d:%02d:%02d] Turtle has eaten %.2f g of food.\n",
                    now.hour(), now.minute(), now.second(), grams_eaten);

      // Push to ThingSpeak
      sendToThingSpeak(totalFoodStock - foodDispensed,
                      totalWaterStock - waterDispensed,
                      grams_corrected);

      // Reset scheduling
      hx711Scheduled = false;
      lastHX711CountdownSec = -1;
    }
  }
}


// ======================================================
// Command Handling
// ======================================================
void handleCommand(const String& cmd) {
  // ----- Tare -----
  if (cmd.equalsIgnoreCase("clear")) {
    Serial.println("Taring... remove any weight.");
    delay(800);
    scale.tare();
    Serial.println("Tare complete.");
    return;
  }

  // ----- Adjust offset -----
  if (cmd.startsWith("adjust")) {
    String arg = cmd.substring(6); arg.trim();
    user_adjust_g = arg.toFloat();
    Serial.printf("Adjust offset set to %.2f g\n", user_adjust_g);
    return;
  }

  // ----- Calibration factor -----
  if (cmd.startsWith("cal")) {
    String arg = cmd.substring(3); arg.trim();
    calibration_factor = arg.toFloat();
    scale.set_scale(calibration_factor);
    Serial.printf("Calibration factor set to %.6f\n", calibration_factor);
    return;
  }

  // ----- Invert calibration factor -----
  if (cmd.equalsIgnoreCase("invert")) {
    calibration_factor = -calibration_factor;
    scale.set_scale(calibration_factor);
    Serial.printf("Calibration factor inverted. New factor: %.6f\n", calibration_factor);
    return;
  }

  // ----- Manually set RTC time (HH:MM:SS) -----
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

  // ----- Logging commands -----
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
          end = DateTime(now.year(), now.month(), now.day() + 1, eh, em, es);
        }

        // ✅ Instead of blocking, schedule HX711 log
        hx711StartDT = start;
        hx711EndDT   = end;
        hx711Scheduled = true;
        lastHX711CountdownSec = -1;

        Serial.printf("HX711 log scheduled from %02d:%02d:%02d to %02d:%02d:%02d\n",
                      start.hour(), start.minute(), start.second(),
                      end.hour(), end.minute(), end.second());
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
      logFood(waitMs);  // duration-based logging still uses blocking function
    }
    return;
  }

  // ----- Unknown command -----
  Serial.println("Unknown command. Use: clear | adjust <grams> | cal <factor> | invert | log <time> | log HH:MM:SS-HH:MM:SS | settime HH:MM:SS");
}

// ======================================================
// Parse duration string like "10s" or "1m" into milliseconds
// ======================================================
unsigned long parseDuration(const String& s) {
  if (s.endsWith("s")) {
    int val = s.substring(0, s.length() - 1).toInt();
    return (unsigned long)val * 1000UL;
  }
  if (s.endsWith("m")) {
    int val = s.substring(0, s.length() - 1).toInt();
    return (unsigned long)val * 60000UL;
  }
  return 0;
}

// ======================================================
// RTC Logging (before/after weight)
// ======================================================
void logFoodRTC(DateTime start, DateTime end) {
  Serial.printf("HX711 log scheduled from %02d:%02d:%02d to %02d:%02d:%02d\n",
                start.hour(), start.minute(), start.second(),
                end.hour(), end.minute(), end.second());

  // ===== Countdown until start =====
  long lastCountdownSec = -1;
  while (rtc.now() < start) {
    DateTime now = rtc.now();
    TimeSpan remaining = start - now;
    long remainingSec = remaining.totalseconds();
    if (remainingSec < 0) remainingSec = 0;

    if (remainingSec != lastCountdownSec) {
      Serial.printf("HX711 log will start in %ld seconds...\n", remainingSec);
      lastCountdownSec = remainingSec;
    }
    delay(500);
  }

  // ===== Before weight =====
  float before = scale.get_units(20);
  if (fabs(before) < DEADBAND) before = 0.0f;
  before += user_adjust_g;
  DateTime now = rtc.now();
  Serial.printf("[%02d:%02d:%02d] Weight at start: %.2f g\n",
                now.hour(), now.minute(), now.second(), before);

  // Auto-set delivered amount if not set
  if (deliveredAmount <= 0.01f) {
    deliveredAmount = before;
    Serial.printf("[%02d:%02d:%02d] Delivered amount auto-set to %.2f g\n",
                  now.hour(), now.minute(), now.second(), deliveredAmount);
  }

  // ===== Countdown until end =====
  lastCountdownSec = -1;
  while (rtc.now() < end) {
    DateTime now2 = rtc.now();
    TimeSpan remaining = end - now2;
    long remainingSec = remaining.totalseconds();
    if (remainingSec < 0) remainingSec = 0;

    if (remainingSec != lastCountdownSec) {
      Serial.printf("HX711 log will finish in %ld seconds...\n", remainingSec);
      lastCountdownSec = remainingSec;
    }
    delay(500);
  }

  // ===== After weight =====
  float after = scale.get_units(20);
  if (fabs(after) < DEADBAND) after = 0.0f;
  after += user_adjust_g;
  now = rtc.now();
  Serial.printf("[%02d:%02d:%02d] Weight at end: %.2f g\n",
                now.hour(), now.minute(), now.second(), after);

  // ===== Difference = amount eaten =====
  float grams_eaten = before - after;
  if (grams_eaten < 0) grams_eaten = 0.0f;
  grams_corrected = grams_eaten;   // store amount eaten

  Serial.printf("[%02d:%02d:%02d] Turtle has eaten %.2f g of food.\n",
                now.hour(), now.minute(), now.second(), grams_eaten);
  // ===== Push to ThingSpeak =====
  sendToThingSpeak(totalFoodStock - foodDispensed,
                   totalWaterStock - waterDispensed,
                   grams_corrected);
}

// ======================================================
// Process Feed/Water/RTC Commands
// ======================================================
void processCommand(String cmd) {
  cmd.trim();
  Serial.printf("Received command: '%s'\n", cmd.c_str());

  if (cmd.length() < 2) return;

  char type = cmd.charAt(0);

  // ----- Set RTC Date/Time -----
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

  // ----- Parse Feed/Water Commands -----
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

  bool isFood  = (type == 'f' || type == 'F');
  bool isWater = (type == 'w' || type == 'W');
  if (!isFood && !isWater) {
    Serial.println("Unknown command type. Use 'f' or 'w'.");
    return;
  }

  // ----- Absolute Time Scheduling -----
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
      foodAmount   = amount;
      foodScheduled = true;
      lastFoodCountdownSec = -1;
      Serial.printf("Food servo scheduled at %02d:%02d:%02d for %.2f g.\n", hh, mm, ss, amount);
    } else {
      waterTargetDT = candidate;
      waterAmount   = amount;
      waterScheduled = true;
      lastWaterCountdownSec = -1;
      Serial.printf("Water servo scheduled at %02d:%02d:%02d for %.2f mL.\n", hh, mm, ss, amount);
    }
  }

  // ----- Delay Scheduling (seconds) -----
  else {
    int delaySeconds = timeOrDelay.toInt();
    if (delaySeconds <= 0) {
      Serial.println("Invalid delay value.");
      return;
    }

    unsigned long nowMillis   = millis();
    unsigned long triggerTime = nowMillis + (unsigned long)delaySeconds * 1000UL;
    unsigned long durationMs;

    if (isFood) {
      durationMs = (unsigned long)((amount / gramsPerSecond) * 1000.0);
      foodTriggerTime    = triggerTime;
      foodReturnTime     = triggerTime + durationMs;
      foodDispenseAmount = amount;
      Serial.printf("Food servo scheduled in %d s to dispense %.2f g.\n", delaySeconds, amount);
    } else {
      durationMs = (unsigned long)((amount / mlPerSecond) * 1000.0);
      waterTriggerTime    = triggerTime;
      waterReturnTime     = triggerTime + durationMs;
      waterDispenseAmount = amount;
      Serial.printf("Water servo scheduled in %d s to dispense %.2f mL.\n", delaySeconds, amount);
    }
  }
}

// ======================================================
// Push Data to ThingSpeak
// ======================================================
void sendToThingSpeak(float foodRemaining, float waterRemaining, float liveWeight) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = String(tsUpdateURL) +
                 "?api_key=" + writeApiKey +
                 "&field1=" + String(foodRemaining, 2) +
                 "&field2=" + String(waterRemaining, 2) +
                 "&field3=" + String(liveWeight, 2);

    http.begin(url);
    int httpCode = http.GET();
    if (httpCode > 0) {
      Serial.printf("ThingSpeak response: %d\n", httpCode);
    } else {
      Serial.printf("ThingSpeak update failed, error: %s\n",
                    http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    Serial.println("Wi-Fi not connected, cannot send to ThingSpeak");
  }
}

// ======================================================
// Perform Before/After Logging by Duration
// ======================================================
void logFood(unsigned long waitMs) {
  DateTime now = rtc.now();
  Serial.printf("[%02d:%02d:%02d] Starting food log (duration %lus)...\n",
                now.hour(), now.minute(), now.second(), waitMs / 1000);

  // ----- Before weight -----
  float before = scale.get_units(20);
  if (fabs(before) < DEADBAND) before = 0.0f;
  before += user_adjust_g;
  Serial.printf("[%02d:%02d:%02d] Before weight: %.2f g\n",
                now.hour(), now.minute(), now.second(), before);

  // Auto-set delivered amount if not set
  if (deliveredAmount <= 0.01) {
    deliveredAmount = before;
    Serial.printf("[%02d:%02d:%02d] Delivered amount auto-set to %.2f g\n",
                  now.hour(), now.minute(), now.second(), deliveredAmount);
  }

  // ----- Wait -----
  delay(waitMs);

  // ----- After weight -----
  float after = scale.get_units(20);
  if (fabs(after) < DEADBAND) after = 0.0f;
  after += user_adjust_g;
  now = rtc.now();
  Serial.printf("[%02d:%02d:%02d] After weight: %.2f g\n",
                now.hour(), now.minute(), now.second(), after);

  // ----- Difference = amount eaten -----
  float grams_eaten = before - after;
  if (grams_eaten < 0) grams_eaten = 0.0f;
  grams_corrected = grams_eaten;   // store amount eaten

  Serial.printf("[%02d:%02d:%02d] Turtle has eaten %.2f g of food.\n",
                now.hour(), now.minute(), now.second(), grams_eaten);

  // ----- Push to ThingSpeak -----
  sendToThingSpeak(totalFoodStock - foodDispensed,
                   totalWaterStock - waterDispensed,
                   grams_corrected);
}
// ===== TalkBack fetch and dispatch =====
void fetchAndExecuteTalkBack() {
  HTTPClient http;
  String postData = "api_key=" + String(writeApiKey) +
                    "&talkback_key=" + String(talkbackKey);

  Serial.println("[ThingSpeak] Polling TalkBack...");
  http.begin(tsUpdateURL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int httpCode = http.POST(postData);
  if (httpCode > 0) {
    String payload = http.getString();
    Serial.printf("[ThingSpeak] HTTP %d, payload: %s\n", httpCode, payload.c_str());
    handleTalkBackCommand(payload);
  } else {
    Serial.printf("[ThingSpeak] POST failed: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

// ===== Parse TalkBack command =====
void handleTalkBackCommand(String cmdRaw) {
  String cmd = cmdRaw;
  cmd.trim();
  if (cmd.length() == 0 || cmd == "0") {
    Serial.println("[TalkBack] No command to execute.");
    return;
  }

  Serial.printf("[TalkBack] Received command: %s\n", cmd.c_str());

  int spaceIndex = cmd.indexOf(' ');
  if (spaceIndex == -1) {
    Serial.println("[TalkBack] Invalid format. Use 'Food X' or 'Water X'");
    return;
  }

  String keyword = cmd.substring(0, spaceIndex);
  String valueStr = cmd.substring(spaceIndex + 1);
  float amount = valueStr.toFloat();
  if (amount <= 0) {
    Serial.println("[TalkBack] Invalid amount.");
    return;
  }

  unsigned long nowMillis = millis();
  unsigned long durationMs;

  if (keyword.equalsIgnoreCase("Food")) {
    durationMs = (unsigned long)((amount / gramsPerSecond) * 1000.0);
    foodTriggerTime    = nowMillis;            // trigger immediately
    foodReturnTime     = nowMillis + durationMs;
    foodDispenseAmount = amount;
    foodServo.write(0); // open
    Serial.printf("Food gate opened via TalkBack for %.2f g (%.0f ms)\n", amount, (float)durationMs);
  } else if (keyword.equalsIgnoreCase("Water")) {
    durationMs = (unsigned long)((amount / mlPerSecond) * 1000.0);
    waterTriggerTime    = nowMillis;           // trigger immediately
    waterReturnTime     = nowMillis + durationMs;
    waterDispenseAmount = amount;
    waterServo.write(0); // open
    Serial.printf("Water gate opened via TalkBack for %.2f mL (%.0f ms)\n", amount, (float)durationMs);
  } else {
    Serial.printf("[TalkBack] Unknown command keyword: %s\n", keyword.c_str());
  }
}
