#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ===== Wi-Fi settings =====
const char* ssid     = "SnakeSheep";
const char* password = "EE3070P1615";

// ===== ThingSpeak TalkBack settings =====
const char* writeApiKey = "LRT1SV5NLOOBPDKV";   // Channel Write API Key
const char* talkbackKey = "U5DD6VCEZO0CIZEZ";   // TalkBack API Key
const char* tsUpdateURL = "https://api.thingspeak.com/update";

// ===== Hardware =====
Servo foodServo;
Servo waterServo;

const int foodPin  = 18;
const int waterPin = 17;

// Gate angles (adjust to your mechanism)
const int FOOD_OPEN_ANGLE   = 45;
const int FOOD_CLOSED_ANGLE = 0;
const int WATER_OPEN_ANGLE  = 0;
const int WATER_CLOSED_ANGLE= 90;

// Calibration constants
float gramsPerSecond = 6.0;   // food dispensing rate (g/s)
float mlPerSecond    = 20.0;  // water dispensing rate (mL/s)

// Timing
unsigned long foodReturnTime  = 0;
unsigned long waterReturnTime = 0;

// TalkBack polling
const unsigned long pollMs = 15000; // >=15s per ThingSpeak limits
unsigned long lastPoll = 0;

void setup() {
  Serial.begin(115200);
  delay(500);

  foodServo.attach(foodPin, 500, 2400);
  waterServo.attach(waterPin, 500, 2400);
  foodServo.write(FOOD_CLOSED_ANGLE);
  waterServo.write(WATER_CLOSED_ANGLE);

  // Connect Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi connected");

  Serial.println("=== Smart Feeder Control (TalkBack) ===");
  Serial.println("TalkBack commands:");
  Serial.println("  Food X   -> dispense X grams");
  Serial.println("  Water X  -> dispense X mL");
  Serial.println("=======================================");
}

void loop() {
  unsigned long nowMillis = millis();

  // Poll TalkBack
  if (WiFi.status() == WL_CONNECTED) {
    if (nowMillis - lastPoll >= pollMs) {
      lastPoll = nowMillis;
      fetchAndExecuteTalkBack();
    }
  } else {
    WiFi.reconnect();
  }

  // Close food gate after duration
  if (foodReturnTime > 0 && nowMillis >= foodReturnTime) {
    foodServo.write(FOOD_CLOSED_ANGLE);
    Serial.println("Food gate closed");
    foodReturnTime = 0;
  }

  // Close water gate after duration
  if (waterReturnTime > 0 && nowMillis >= waterReturnTime) {
    waterServo.write(WATER_CLOSED_ANGLE);
    Serial.println("Water gate closed");
    waterReturnTime = 0;
  }
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

  // Split into keyword + value
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

  unsigned long durationMs;

  if (keyword.equalsIgnoreCase("Food")) {
    durationMs = (unsigned long)((amount / gramsPerSecond) * 1000.0);
    foodServo.write(FOOD_OPEN_ANGLE);
    foodReturnTime = millis() + durationMs;
    Serial.printf("Food gate opened for %.2f g (%.0f ms)\n", amount, (float)durationMs);
  } else if (keyword.equalsIgnoreCase("Water")) {
    durationMs = (unsigned long)((amount / mlPerSecond) * 1000.0);
    waterServo.write(WATER_OPEN_ANGLE);
    waterReturnTime = millis() + durationMs;
    Serial.printf("Water gate opened for %.2f mL (%.0f ms)\n", amount, (float)durationMs);
  } else {
    Serial.printf("[TalkBack] Unknown command keyword: %s\n", keyword.c_str());
  }
}
