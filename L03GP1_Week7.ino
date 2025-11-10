#include <WiFi.h>
#include "ThingSpeak.h"
#include "secrets.h"
#include "time.h"

// WiFi init
const char* ssid = SECRET_SSID;
const char* pass = SECRET_PASS;
WiFiClient client;
// Loop init
unsigned long lastTime = 0;
unsigned long timerDelay = 15000;
// ThingSpeak init
float temperatureC;
unsigned long myChannelNumber = SECRET_CH_ID;
const char* myWriteAPIKey = SECRET_WRITE_APIKEY;
// NTP init
const char* ntpServer = "stdtime.gov.hk";
const long gmtOffset_sec = 28800;
const int daylightOffset_sec = 0;
struct tm timeinfo;
// temperature probe
const int inPin = 1;


//==================== Setup ========================
void setup() {
  Serial.begin(115200);  //Initialize serial
  pinMode(inPin, INPUT);

  WiFi.mode(WIFI_STA);
  // Connect or reconnect to WiFi
  checkWifi(); 

  // Initialize ThingSpeak
  ThingSpeak.begin(client);
  // Initialize and get time from NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}
//================= Main Loop ========================
void loop() {
  
  if ((millis() - lastTime) > timerDelay) {
    checkWifi();
    if (!getLocalTime(&timeinfo)) Serial.println("failed to get time.");
    else Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
    // Get a new temperature reading
    temperatureC = analogRead(inPin) / 10;  //random(15,32);
    Serial.print("Temperature (ºC): ");
    Serial.println(temperatureC);
    writeTS(ThingSpeak.writeField(myChannelNumber, 1, temperatureC, myWriteAPIKey));
    lastTime = millis();
  }
}







//====================== Functions =========================
void writeTS(int x) {
  if (x == 200) Serial.println("Channel update successful.");
  else Serial.println("Problem updating channel. HTTP error code " + String(x));
}

void checkWifi() {
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print("Attempting to connect to ");
    Serial.print(ssid);
    WiFi.begin(ssid, pass);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nConnected.");
    delayMicroseconds(1);
  }
}