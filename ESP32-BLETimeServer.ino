/*

  ESP32 BLETimeServer - A time-synchronization tool for ESP32-BLEcollector
  Source: https://github.com/tobozo/ESP32-BLETimeServer

  MIT License

  Copyright (c) 2018 tobozo

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

  -----------------------------------------------------------------------------

  This code is heavily based on the examples found on this repo:
    https://github.com/nkolban/esp32-snippets/blob/master/cpp_utils/tests/BLETests/Arduino/
  Big thanks to @chegewara for providing support on this.

*/
#include <WiFi.h>
#include "BLEDevice.h"
#include "BLEServer.h"
#include "BLE2902.h"
#include <sys/time.h>
#include "lwip/apps/sntp.h"

#define TICKS_TO_DELAY 1000
//#define WIFI_SSID "my_wifi_ssid"
//#define WIFI_PASSWD "my_wifi_password"

#ifdef WIFI_SSID
  #define WiFi_Begin() WiFi.begin(WIFI_SSID, WIFI_PASSWD);
#else
  #define WiFi_Begin() WiFi.begin();
#endif

const char* NTP_SERVER = "europe.pool.ntp.org";
static void getNTPTime(void);
static void initNTP(void);
static void initWiFi(void);
static bool WiFiConnect();

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minutes;
    uint8_t seconds;
    uint8_t wday;
    uint8_t fraction;
    uint8_t adjust = 0;
} bt_time_t;

BLEService* pService;
BLEServer* pServer;

class TimeServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) {
    BLEDevice::startAdvertising();
  }
  void onDisconnect(BLEServer *pServer) { 
    ;
  }
};


void timeServerTask(void *p) {
  TickType_t lastWaketime;
  lastWaketime = xTaskGetTickCount();
  struct timeval tv;
  bt_time_t _time;
  struct tm* _t;
  while(1){
    gettimeofday(&tv, nullptr);
    _t = localtime(&(tv.tv_sec));
    _time.year = 1900 + _t->tm_year;
    _time.month = _t->tm_mon + 1;
    _time.wday = _t->tm_wday == 0 ? 7 : _t->tm_wday;
    _time.day = _t->tm_mday;
    _time.hour = _t->tm_hour;
    _time.minutes = _t->tm_min;
    _time.seconds = _t->tm_sec;
    _time.fraction = tv.tv_usec * 256 /1000000;
    ((BLECharacteristic*)p)->setValue((uint8_t*)&_time, sizeof(bt_time_t));
    ((BLECharacteristic*)p)->notify();
    // send notification with date/time exactly every TICKS_TO_DELAY ms
    vTaskDelayUntil(&lastWaketime, TICKS_TO_DELAY/portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}


void TimeServerStart() {
  Serial.println("Starting BLE Time Server");
  BLEDevice::init("TimeServer-UTC");
  BLEDevice::setMTU(50);
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new TimeServerCallbacks());
  pService = pServer->createService(BLEUUID((uint16_t)0x1805));

  BLECharacteristic* pCharacteristic = pService->createCharacteristic(
    BLEUUID((uint16_t)0x2a2b),
    BLECharacteristic::PROPERTY_NOTIFY   |
    BLECharacteristic::PROPERTY_READ 
  );
  BLE2902* p2902Descriptor = new BLE2902();
  p2902Descriptor->setNotifications(true);
  pCharacteristic->addDescriptor(p2902Descriptor);
  pService->start();

  pAdvertising->addServiceUUID(BLEUUID((uint16_t)0x1805));
  pAdvertising->setMinInterval(0x100);
  pAdvertising->setMaxInterval(0x200);

  BLEDevice::startAdvertising();
  xTaskCreate(timeServerTask, "timeServerTask", 4096, (void*)pCharacteristic, 6, NULL);

  Serial.println("Advertising started");
}


static void getNTPTime(void) {
  if(!WiFiConnect()) {
    ESP.restart();
  }
  initNTP();
  time_t now = 0;
  struct tm timeinfo = {};
  int retry = 0;
  const int retry_count = 10;
  while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
    Serial.printf("Waiting for system time to be set... (%d/%d)\n", retry, retry_count);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    time(&now);
    localtime_r(&now, &timeinfo);
  }
}

static void initNTP(void) {
  Serial.println("Initializing SNTP");
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, (char*)NTP_SERVER);
  sntp_init();
}

// very stubborn wifi connect
static bool WiFiConnect() {
  unsigned long init_time = millis();
  unsigned long last_attempt = millis();
  unsigned long max_wait = 10000;
  byte attempts = 5;
  btStop();
  WiFi_Begin();
  while(WiFi.status() != WL_CONNECTED && attempts>0) {
    if( last_attempt + max_wait < millis() ) {
      attempts--;            
      last_attempt = millis();
      Serial.println( String("[WiFi] Restarting ("+String(attempts)+" attempts left)").c_str() );
      WiFi.mode(WIFI_OFF);
      WiFi_Begin();
    }
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println( String("[WiFi] Got an IP Address:" + WiFi.localIP().toString()).c_str() );
  } else {
    Serial.println("[WiFi] No IP Address");
  }
  return WiFi.status() == WL_CONNECTED;
}

void setup(void) {
  //esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
  Serial.begin(115200);
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  // Is time set? If not, tm_year will be (1970 - 1900).
  if (timeinfo.tm_year < (2016 - 1900)) {
    Serial.println("Time is not set yet. Connecting to WiFi and getting time over NTP.");
    getNTPTime();
    // update 'now' variable with current time
    time(&now);
  }
  TimeServerStart();
}


void loop() {
  vTaskSuspend(NULL);
}
