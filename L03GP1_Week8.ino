#include "Arduino.h"
#include "WiFi.h"
#include "Audio.h"
#include "secrets.h"
#define MAX98357A_I2S_DOUT 13
#define MAX98357A_I2S_BCLK 12
#define MAX98357A_I2S_LRC 11
// Gain -> GND or 3V3

const char* ssid = SECRET_SSID;
const char* pass = SECRET_PASS;
WiFiClient client;
Audio audio;

void my_audio_info(Audio::msg_t m) {
    Serial.printf("%s: %s\n", m.s, m.msg);
}

void set_Player() {
  while (WiFi.status() != WL_CONNECTED) delay(1500);
  audio.setPinout(MAX98357A_I2S_BCLK, MAX98357A_I2S_LRC, MAX98357A_I2S_DOUT);
  audio.setVolume(10);
  audio.connecttohost("http://starfish-home.mynetgear.com:8000/domlive.m3u");
}
//http://vis.media-ice.musicradio.com/CapitalMP3
//http://37.59.37.173:44588/;?uGjM_8Fp5X2AVXj2Opa__Tg_QrKDg0SBs5sW9NWTqDgvpOMH
//http://s5.myradiostream.com:44588/;?

void setup() {
  // put your setup code here, to run once:
  Audio::audio_info_callback = my_audio_info;
  Serial.begin(115200);
  WiFi.begin(ssid, pass);
  set_Player(); 
} 

void loop() {
  // put your main code here, to run repeatedly:
  audio.loop();
  if (WiFi.status() != WL_CONNECTED) set_Player();
  vTaskDelay(1);
}
