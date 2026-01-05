#include <PZEM004Tv30.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TM1637Display.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <DS1307RTC.h>
#include <TimeLib.h>

// ----------------- Firebase -----------------
const char* firebaseHost   = "sky2-9d324-default-rtdb.firebaseio.com";
const char* firebaseHost1  = "anupam-32ea7-default-rtdb.firebaseio.com";
String getPath             = "/user/fTdRgWx4YNVtEVTOzO1GAltFv9G3/motorSchedule.json";
String statusPostPath      = "/user/uid/1001/motorStatusLog.json";
String HeartbeatPostPath   = "/user/uid/1001/heartbeat.json";

// ----------------- NTP -----------------
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800); // IST offset

// ----------------- PZEM -----------------
SoftwareSerial pzemSerial(D5, D6); // RX, TX
PZEM004Tv30 pzem1(pzemSerial);
float VOLTAGE, CURRENT, POWER;
unsigned long lastVoltageRead = 0;
const unsigned long VOLTAGE_READ_INTERVAL = 2000;

// ----------------- Display -----------------
#define CLK D3
#define DIO D4
TM1637Display display(CLK, DIO);
uint8_t blank[] = {0x00,0x00,0x00,0x00};

// ----------------- Pins -----------------
const int ot_sensor       = D1;
const int buzzer          = D8;
const int ot_status       = D7;
const int auto_status     = D2;
const int input1          = D9;
const int wifiBtnThreshold = 100;

// ----------------- EEPROM -----------------
#define EEPROM_SIZE 512
#define SCHED1_ADDR_HOUR 10
#define SCHED1_ADDR_MIN 11
#define SCHED1_ADDR_DURATION 12
#define SCHED2_ADDR_HOUR 20
#define SCHED2_ADDR_MIN 21
#define SCHED2_ADDR_DURATION 22
#define SCHED3_ADDR_HOUR 30
#define SCHED3_ADDR_MIN 31
#define SCHED3_ADDR_DURATION 32
#define MOTOR_DURATION_ADDR 100
#define MOTOR_REMAIN_ADDR 110
#define BLOCKED_FLAGS_ADDR 200

// ----------------- Variables -----------------
int motor_duration = 30;
int motor_time = 0;                    // Remaining seconds
bool motor_status = false;
int currentScheduleIndex = -1;
bool scheduleCompleted[3] = {false,false,false};
bool scheduleBlocked[3] = {false,false,false};
int numSchedules = 0;
bool manualMode = true;

unsigned long lastFirebaseFetchTime = 0;
const unsigned long firebaseFetchInterval = 60000;
const unsigned long wifiReconnectInterval = 15000;
unsigned long lastWiFiReconnectAttempt = 0;
const unsigned long HEARTBEAT_INTERVAL = 30000;
unsigned long lastHeartbeatTime = 0;
const unsigned long VOLTAGE_SEND_INTERVAL = 30000;
unsigned long lastVoltageSend = 0;

// Non-blocking motor control
unsigned long motorEndMillis = 0;
unsigned long lastMotorEEPROMSave = 0;
unsigned long otLowSince = 0;

// ----------------- WiFi Manager -----------------
WiFiManager wm;

// ----------------- Helper -----------------
float zeroIfNan(float v) { return isnan(v)?0:v; }

void readVoltage() {
  VOLTAGE = zeroIfNan(pzem1.voltage());
  CURRENT = zeroIfNan(pzem1.current());
  POWER   = zeroIfNan(pzem1.power());
}

void stopMotor(bool blocked=true, String reason="manual") {
  motor_status = false;
  digitalWrite(buzzer, LOW);
  digitalWrite(auto_status, LOW);
  display.showNumberDec(0, false);
  motor_time = motor_duration * 60;

  Serial.printf("Motor stopped (%s)\n", reason.c_str());

  if(currentScheduleIndex != -1){
    if(blocked) scheduleBlocked[currentScheduleIndex] = true;
    scheduleCompleted[currentScheduleIndex] = true;
  }

  // Save remaining time & blocked flags to EEPROM
  EEPROM.write(MOTOR_REMAIN_ADDR, motor_time / 60);
  for(int i=0;i<3;i++) EEPROM.write(BLOCKED_FLAGS_ADDR+i, scheduleBlocked[i]);
  EEPROM.commit();

  // Log motor status to Firebase if connected
  if(WiFi.status() == WL_CONNECTED){
    WiFiClientSecure c; c.setInsecure();
    HTTPClient https;
    String url = String("https://") + firebaseHost1 + statusPostPath;
    if(https.begin(c,url)){
      https.addHeader("Content-Type","application/json");
      timeClient.update();
      time_t raw = timeClient.getEpochTime();
      struct tm *t = localtime(&raw);
      char ts[40]; strftime(ts,sizeof(ts),"%d %b %Y %I:%M:%S %p",t);
      String body = "{\"motor_status\":0,\"timestamp\":\""+String(ts)+"\",\"reason\":\""+reason+"\"}";
      int code = https.PUT(body);
      Serial.printf("Motor status PUT code: %d\n",code);
      https.end();
    }
  }

  currentScheduleIndex = -1;
}

void startMotor(int durationSec, String reason="auto") {
  motor_status = true;
  motor_time = durationSec;
  motorEndMillis = millis() + durationSec * 1000UL;
  digitalWrite(buzzer, HIGH);
  digitalWrite(auto_status, HIGH);
  Serial.printf("Motor started for %d sec (%s)\n", durationSec, reason.c_str());
}

void updateMotor() {
  if(!motor_status) return;
  long remainingSec = (long)(motorEndMillis - millis()) / 1000;
  motor_time = remainingSec > 0 ? remainingSec : 0;

  int disp = (motor_time / 60) * 100 + (motor_time % 60);
  display.showNumberDecEx(disp, 0b01000000);

  if(motor_time <= 0) {
    stopMotor(true, "schedule_end");
    return;
  }

  if(millis() - lastMotorEEPROMSave > 5000){
    EEPROM.write(MOTOR_REMAIN_ADDR, motor_time / 60);
    EEPROM.commit();
    lastMotorEEPROMSave = millis();
  }
}

// ----------------- Schedule -----------------
void readSchedule(int idx,int &h,int &m,int &d){
  int base = (idx==0?SCHED1_ADDR_HOUR:(idx==1?SCHED2_ADDR_HOUR:SCHED3_ADDR_HOUR));
  h = EEPROM.read(base); m = EEPROM.read(base+1); d = EEPROM.read(base+2);
  if(h>23||h==255) h=0;
  if(m>59||m==255) m=0;
  if(d<1||d>240||d==255) d=30;
}

int findMatchingSchedule(int ch,int cm,int &remainSec){
  int nowSec = ch*3600 + cm*60;
  for(int i=0;i<numSchedules;i++){
    int h,m,d; readSchedule(i,h,m,d);
    int startSec = h*3600 + m*60;
    int endSec = startSec + d*60;
    if(nowSec>=startSec && nowSec<endSec){
      remainSec = endSec - nowSec;
      return i;
    }
  }
  remainSec = 0;
  return -1;
}

// ----------------- Firebase -----------------
void getScheduleFromFirebase(){
    if(WiFi.status()!=WL_CONNECTED){
        Serial.println("WiFi not connected, cannot fetch schedule");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    String url = String("https://") + firebaseHost + getPath;
    if(!https.begin(client, url)){
        Serial.println("Failed to begin HTTPS connection");
        return;
    }

    int code = https.GET();
    if(code != 200){
        Serial.printf("GET failed, code: %d\n", code);
        https.end();
        return;
    }

    String payload = https.getString();
    Serial.println("Firebase response:");
    Serial.println(payload);

    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if(err){
        Serial.print("JSON parse error: ");
        Serial.println(err.c_str());
        https.end();
        return;
    }

    if(!doc.containsKey("schedules")){
        Serial.println("JSON does not contain 'schedules' key!");
        https.end();
        return;
    }

    JsonArray arr = doc["schedules"];
    numSchedules = arr.size();
    Serial.printf("Found %d schedules\n", numSchedules);

    for(int i=0;i<numSchedules;i++){
        int sh = arr[i]["startHour"] | 0;
        int sm = arr[i]["startMin"] | 0;
        int du = arr[i]["duration"] | 30;

        int base = (i==0 ? SCHED1_ADDR_HOUR : (i==1 ? SCHED2_ADDR_HOUR : SCHED3_ADDR_HOUR));
        EEPROM.write(base, sh);
        EEPROM.write(base+1, sm);
        EEPROM.write(base+2, du);

        // Reset blocked flags for updated schedules
        scheduleBlocked[i] = false;
        EEPROM.write(BLOCKED_FLAGS_ADDR+i, 0);

        Serial.printf("Schedule %d -> %02d:%02d, duration %d min\n", i+1, sh, sm, du);
    }
    EEPROM.commit();
    Serial.println("Schedules updated to EEPROM and blocked flags cleared");

    https.end();
}

void sendHeartbeat(){
  if(WiFi.status()!=WL_CONNECTED) return;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient https;
  String url = String("https://")+firebaseHost1+HeartbeatPostPath;
  if(!https.begin(c,url)) return;
  https.addHeader("Content-Type","application/json");
  timeClient.update();
  time_t raw = timeClient.getEpochTime();
  struct tm *t = localtime(&raw);
  char ts[40]; strftime(ts,sizeof(ts),"%d %b %Y %I:%M:%S %p",t);
  String body="{\"last_active\":\""+String(ts)+"\"}";
  int code = https.PUT(body);
  Serial.printf("Heartbeat PUT code: %d\n",code);
  https.end();
}

void sendVoltageData(){
  if(WiFi.status()!=WL_CONNECTED) return;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient https;
  String url = String("https://")+firebaseHost1+"/user/uid/1001/pzem.json";
  if(!https.begin(c,url)) return;
  https.addHeader("Content-Type","application/json");
  String body="{\"voltage\":"+String(VOLTAGE,2)+",\"current\":"+String(CURRENT,2)+",\"power\":"+String(POWER,2)+"}";
  int code = https.PUT(body);
  Serial.printf("PZEM data POST code: %d\n",code);
  https.end();
}

// ----------------- Setup -----------------
void setup(){
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  display.setBrightness(0x0f);
  display.setSegments(blank);

  pinMode(input1,INPUT_PULLUP);
  pinMode(buzzer,OUTPUT);
  pinMode(auto_status,OUTPUT);
  pinMode(ot_status,OUTPUT);
  pinMode(ot_sensor,INPUT_PULLUP);

  digitalWrite(buzzer,LOW);
  digitalWrite(auto_status,LOW);
  digitalWrite(ot_status,LOW);

  int dur = EEPROM.read(MOTOR_DURATION_ADDR);
  motor_duration = (dur<=0||dur>240||dur==255)?30:dur;

  int rem = EEPROM.read(MOTOR_REMAIN_ADDR);
  motor_time = (rem>0 && rem<14400)? rem*60 : motor_duration*60;

  // Load blocked flags
  for(int i=0;i<3;i++){
    scheduleBlocked[i] = EEPROM.read(BLOCKED_FLAGS_ADDR+i);
  }
  Serial.printf("Loaded blocked flags: [%d,%d,%d]\n", scheduleBlocked[0], scheduleBlocked[1], scheduleBlocked[2]);

  timeClient.begin();

  if(WiFi.SSID()!=""){
    WiFi.begin();
    Serial.println("Connecting saved WiFi...");
    unsigned long st=millis();
    while(WiFi.status()!=WL_CONNECTED && millis()-st<5000) delay(200);
    if(WiFi.status()==WL_CONNECTED){
      Serial.println("Connected at setup!");
      manualMode=false;
      getScheduleFromFirebase();
    }
  }
}

void loop() {
  unsigned long now = millis();

  // ----------------- Voltage read -----------------
  if(now - lastVoltageRead > VOLTAGE_READ_INTERVAL){
    lastVoltageRead = now;
    readVoltage();
  }

  // ----------------- OT sensor -----------------
  if(digitalRead(ot_sensor)==LOW){
    if(otLowSince==0) otLowSince=now;
    if(now - otLowSince > 2000 && motor_status) stopMotor(true,"OT sensor");
  } else otLowSince=0;

  // ----------------- Motor update -----------------
  updateMotor();

  // ----------------- Manual button -----------------
  static bool lastBtn = HIGH;
  bool curBtn = digitalRead(input1);
  if(lastBtn==HIGH && curBtn==LOW){
    if(!motor_status){
      startMotor(motor_duration*60,"manual");
    } else stopMotor(true,"manual");
  }
  lastBtn = curBtn;

  // ----------------- WiFi portal -----------------
  static bool lastWiFiBtn=false;
  int analogVal=analogRead(A0);
  bool curWiFiBtn=analogVal<wifiBtnThreshold;
  if(curWiFiBtn && !lastWiFiBtn){
    Serial.println("WiFi portal starting...");
    wm.startConfigPortal("Setup_WiFi","admin123");
    if(WiFi.status()==WL_CONNECTED){
      manualMode=false;
      getScheduleFromFirebase();
    }
  }
  lastWiFiBtn=curWiFiBtn;

  // ----------------- WiFi reconnect -----------------
  if(WiFi.status()!=WL_CONNECTED && now-lastWiFiReconnectAttempt>wifiReconnectInterval){
    if(WiFi.SSID()!=""){ Serial.println("Attempting WiFi reconnect..."); WiFi.begin(); }
    lastWiFiReconnectAttempt=now;
  }

  // ----------------- Heartbeat & voltage -----------------
  if(WiFi.status()==WL_CONNECTED){
    if(now-lastHeartbeatTime>HEARTBEAT_INTERVAL){ sendHeartbeat(); lastHeartbeatTime=now; }
    if(now - lastVoltageSend > VOLTAGE_SEND_INTERVAL) { sendVoltageData(); lastVoltageSend = now; }
  }

  // ----------------- Auto schedule -----------------
  int h,m,remainSec;
  bool timeAvailable=false;
  if(WiFi.status()==WL_CONNECTED && timeClient.update()){ h=timeClient.getHours(); m=timeClient.getMinutes(); timeAvailable=true; }
  else { tmElements_t tm; if(RTC.read(tm)){ h=tm.Hour; m=tm.Minute; timeAvailable=true; } }

  if(timeAvailable){
    int match=findMatchingSchedule(h,m,remainSec);
    if(!manualMode && match!=-1 && !scheduleCompleted[match] && !scheduleBlocked[match] && !motor_status){
      startMotor(remainSec,"auto");
      readSchedule(match,h,m,motor_duration);
      currentScheduleIndex=match;
      scheduleCompleted[match]=true;
    }

    // Reset completed/blocked flags if schedule ended
    for(int i=0;i<numSchedules;i++){
      int sh,sm,sd; readSchedule(i,sh,sm,sd);
      int startSec=sh*3600+sm*60;
      int endSec=startSec+sd*60;
      int nowSec=h*3600+m*60;
      if(nowSec >= endSec){
        scheduleCompleted[i] = false;
        scheduleBlocked[i] = false;
      }
    }

    // Fetch Firebase schedule periodically
    if(!manualMode && WiFi.status()==WL_CONNECTED && millis()-lastFirebaseFetchTime>firebaseFetchInterval){
      getScheduleFromFirebase();
      lastFirebaseFetchTime=millis();
    }
  }
}
