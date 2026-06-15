

#include <Arduino.h>
#include <ArduinoJson.h>
#include <time.h>
#include <TM1637Display.h>
#include <Wire.h>
#include "RTClib.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

#define CLK_PIN   D3
#define DIO_PIN   D5

#define DEVICE_ID_M1   110015
#define DEVICE_ID_M2   110016

const char WIFI_SSID[]    = "anupam";
const char WIFI_PASS[]    = "12345678";
const char USER_EMAIL[]   = "9630852741@gmail.com";
const char USER_PASS[]    = "123456";
const char SUPABASE_URL[] = "https://fkgfdgwpqqfxhnyuwtwe.supabase.co";
const char SUPABASE_KEY[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
  "eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0."
  "Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU";

#define TOKEN_REFRESH_BEFORE_S   120
#define DRIFT_THRESHOLD_S        30
#define SYNC_INTERVAL_MS         3600000UL
#define EXECUTION_INTERVAL_DEF   10000UL
#define HB_TO_MASTER_MS          8000UL

struct MotorState {
  uint32_t startUnix = 0; uint32_t stopUnix = 0;
  int duration = 10; bool sch_enabled = false; bool active = false;
  int state = 0; int ack = 0; int runtimeSecs = 0;
  uint32_t appManualStopUntil = 0; bool otTripped = false;
};
MotorState M1, M2;

String        USER_TOKEN = ""; bool login_status = false;
unsigned long tokenExpiresAt = 0;
unsigned long EXECUTION_INTERVAL = EXECUTION_INTERVAL_DEF;
bool timeSynced = false;
unsigned long lastSyncMs = 0, lastExecMs = 0, lastWifiMs = 0, lastHbToMasterMs = 0;
bool colonBlink = false;
String lastChangeM1 = "", lastChangeM2 = "";
int  masterM1State = -1, masterM2State = -1;
bool masterOT1 = false, masterOT2 = false, masterStatusNew = false;

RTC_DS1307    rtc;
TM1637Display dispObj(CLK_PIN, DIO_PIN);
bool          rtcAvail = false;
String        urlM1, urlM2;

bool httpPatch(const char *url, const char *body);
bool httpGet(const char *url, String &out);
void updateTableDirect(const String &url, int st, int ds);
void updateAck(const String &url, int ack);
void sendHeartbeatCloud(const String &url, int runtimeSecs);
void net_login(); void checkTokenRefresh(); void net_compareAndSyncTime();
bool net_hasCloudChanged(int deviceId, String &lastChange);
void net_getTableData(const String &url, MotorState &M, bool isM1);
void checkSch(MotorState &M, bool isM1); void net_wifiReconnectIfNeeded();

void buildUrls() {
  urlM1 = String(SUPABASE_URL) + "/rest/v1/pump_motor?device_id=eq." + String(DEVICE_ID_M1);
  urlM2 = String(SUPABASE_URL) + "/rest/v1/pump_motor?device_id=eq." + String(DEVICE_ID_M2);
}

time_t getCurrentUnixTime() {
  if (rtcAvail && rtc.isrunning()) return (time_t)rtc.now().unixtime();
  time_t t = time(nullptr); return (t > 1000000000UL) ? t : 0;
}

uint32_t hourMinuteToUnix(uint8_t h, uint8_t m) {
  time_t now = getCurrentUnixTime(); if (now == 0) return 0;
  struct tm t; gmtime_r(&now, &t);
  t.tm_hour = h; t.tm_min = m; t.tm_sec = 0;
  return (uint32_t)mktime(&t);
}

void sendToMaster(const char *cmd) { Serial.println(cmd); }

void parseLineFromMaster(String &line) {
  if (line.length() < 4) return;
  if (!(line.charAt(0)=='S' && line.charAt(1)==':')) return;
  int m1i=line.indexOf("M1:"), m2i=line.indexOf("M2:");
  int ot1i=line.indexOf("OT1:"), ot2i=line.indexOf("OT2:");
  int  newM1=(m1i >=0)?line.substring(m1i+3, m1i+4).toInt():masterM1State;
  int  newM2=(m2i >=0)?line.substring(m2i+3, m2i+4).toInt():masterM2State;
  bool nOT1=(ot1i>=0)?(line.substring(ot1i+4,ot1i+5).toInt()==1):masterOT1;
  bool nOT2=(ot2i>=0)?(line.substring(ot2i+4,ot2i+5).toInt()==1):masterOT2;
  if (newM1!=masterM1State||newM2!=masterM2State||nOT1!=masterOT1||nOT2!=masterOT2) {
    masterM1State=newM1; masterM2State=newM2;
    masterOT1=nOT1; masterOT2=nOT2; masterStatusNew=true;
  }
}

void pollMasterSerial() {
  static String rxBuf = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c=='\n') { rxBuf.trim(); if(rxBuf.length()>0)parseLineFromMaster(rxBuf); rxBuf=""; }
    else if(c!='\r') { rxBuf+=c; if(rxBuf.length()>80)rxBuf=""; }
  }
}

void syncMasterStatusToCloud() {
  if (!masterStatusNew) return;
  if (!login_status || WiFi.status()!=WL_CONNECTED) return;
  masterStatusNew = false;


  if (masterM1State >= 0) {
    M1.state     = masterM1State;
    M1.otTripped = masterOT1;
    updateTableDirect(urlM1, M1.state, M1.state);
  }
  if (masterM2State >= 0) {
    M2.state     = masterM2State;
    M2.otTripped = masterOT2;
    updateTableDirect(urlM2, M2.state, M2.state);
  }
}

void sendHeartbeatToMaster() {
  unsigned long now=millis();
  if (now-lastHbToMasterMs<HB_TO_MASTER_MS) return;
  lastHbToMasterMs=now; sendToMaster("HB");
}

bool httpPatch(const char *url, const char *body) {
  if (WiFi.status()!=WL_CONNECTED) return false;
  WiFiClientSecure cl; cl.setInsecure();
  HTTPClient https; https.setTimeout(4000);
  if (!https.begin(cl,url)) return false;
  https.addHeader(F("apikey"),SUPABASE_KEY);
  https.addHeader(F("Authorization"),"Bearer "+USER_TOKEN);
  https.addHeader(F("Content-Type"),F("application/json"));
  https.addHeader(F("Prefer"),F("return=minimal"));
  int code=https.sendRequest("PATCH",body); https.end();
  return (code>=200&&code<300);
}

bool httpGet(const char *url, String &out) {
  if (WiFi.status()!=WL_CONNECTED) return false;
  WiFiClientSecure cl; cl.setInsecure();
  HTTPClient https; https.setTimeout(4000);
  if (!https.begin(cl,url)) return false;
  https.addHeader(F("apikey"),SUPABASE_KEY);
  https.addHeader(F("Authorization"),"Bearer "+USER_TOKEN);
  https.addHeader(F("Content-Type"),F("application/json"));
  int code=https.GET(); if(code==200) out=https.getString();
  https.end(); return (code==200);
}

void updateTableDirect(const String &url, int st, int ds) {
  String b="{\"state\":"+String(st)+",\"device_state\":"+String(ds)+"}";
  httpPatch(url.c_str(),b.c_str());
}
void updateAck(const String &url, int ack) {
  String b="{\"ack\":"+String(ack)+"}"; httpPatch(url.c_str(),b.c_str());
}
void sendHeartbeatCloud(const String &url, int runtimeSecs) {
  String b="{\"heart_beat_count\":10,\"runtime_secs\":"+String(runtimeSecs)+"}";
  httpPatch(url.c_str(),b.c_str());
}

void net_login() {
  if (WiFi.status()!=WL_CONNECTED) return;
  String authUrl=String(SUPABASE_URL)+"/auth/v1/token?grant_type=password";
  WiFiClientSecure cl; cl.setInsecure(); HTTPClient https; https.setTimeout(8000);
  if (!https.begin(cl,authUrl.c_str())) return;
  https.addHeader(F("apikey"),SUPABASE_KEY);
  https.addHeader(F("Content-Type"),F("application/json"));
  String body="{\"email\":\""+String(USER_EMAIL)+"\",\"password\":\""+String(USER_PASS)+"\"}";
  int code=https.POST(body);
  if (code==200) {
    StaticJsonDocument<1024> doc; String resp=https.getString();
    if (!deserializeJson(doc,resp)&&doc["access_token"].is<const char*>()) {
      USER_TOKEN=doc["access_token"].as<String>();
      unsigned long ei=doc["expires_in"]|3600UL;
      tokenExpiresAt=millis()+((ei-TOKEN_REFRESH_BEFORE_S)*1000UL);
      login_status=true;
    }
  } else { login_status=false; }
  https.end();
}

void checkTokenRefresh() {
  if (!login_status) return;
  if (millis()>=tokenExpiresAt) { login_status=false; net_login(); }
}

void net_compareAndSyncTime() {
  if (timeSynced&&(millis()-lastSyncMs<SYNC_INTERVAL_MS)) return;
  if (WiFi.status()!=WL_CONNECTED) return;
  WiFiClientSecure cl; cl.setInsecure(); HTTPClient https; https.setTimeout(5000);
  if (!https.begin(cl,"https://api.skyiottech.com/time")) return;
  int code=https.GET(); String body=(code==200)?https.getString():""; https.end();
  if (code!=200||body.isEmpty()) return;
  StaticJsonDocument<256> doc; if (deserializeJson(doc,body)) return;
  time_t it=(time_t)doc["unix_time"].as<unsigned long>();
  if (rtcAvail&&rtc.isrunning()) {
    long drift=abs((long)(it-(long)rtc.now().unixtime()));
    if (drift>DRIFT_THRESHOLD_S) rtc.adjust(DateTime((uint32_t)it));
  }
  timeSynced=true; lastSyncMs=millis();
}

bool net_hasCloudChanged(int deviceId, String &lastChange) {
  if (!login_status||WiFi.status()!=WL_CONNECTED) return false;
  String url=String(SUPABASE_URL)+"/rest/v1/pump_motor?device_id=eq."+String(deviceId)+"&select=last_change";
  String resp; if (!httpGet(url.c_str(),resp)) return false;
  StaticJsonDocument<256> doc; if (deserializeJson(doc,resp)) return false;
  const char *lc=doc[0]["last_change"]|""; if (strlen(lc)==0) return true;
  String nv=String(lc); if (nv!=lastChange){lastChange=nv;return true;} return false;
}

void net_getTableData(const String &url, MotorState &M, bool isM1) {
  if (!login_status||WiFi.status()!=WL_CONNECTED) return;
  String resp; if (!httpGet(url.c_str(),resp)) return;
  StaticJsonDocument<512> doc; if (deserializeJson(doc,resp)) return;
  int desiredState=doc[0]["state"]|0; M.ack=doc[0]["ack"]|0;
  M.sch_enabled=(doc[0]["sch1_en"]==1);
  uint32_t duration=doc[0]["sch1_duration"]|10; M.duration=(int)duration;
  int syncDur=doc[0]["sync_duration"]|0;
  if (syncDur>0) EXECUTION_INTERVAL=(unsigned long)syncDur*1000UL;
  if (desiredState==1&&M.ack==1&&M.state==0&&!M.otTripped) {
    sendToMaster(isM1?"M1:1":"M2:1"); M.state=1;
    updateTableDirect(url,1,0); updateAck(url,0);
  }
  if (desiredState==0&&M.ack==1&&M.state==1) {
    sendToMaster(isM1?"M1:0":"M2:0"); M.state=0;
    updateTableDirect(url,0,0); updateAck(url,0);
  }
  const char *schTime=doc[0]["sch1_start"]|"00:00";
  uint16_t h=0,m=0;
  if (schTime&&strlen(schTime)>=5) {
    h=(schTime[0]-'0')*10+(schTime[1]-'0');
    m=(schTime[3]-'0')*10+(schTime[4]-'0');
  }
  uint32_t ns=hourMinuteToUnix((uint8_t)h,(uint8_t)m);
  uint32_t ne=ns+(duration*60UL);
  if (ns!=M.startUnix||ne!=M.stopUnix) {
    M.startUnix=ns; M.stopUnix=ne; M.active=false; M.appManualStopUntil=0;
  }
}

void checkSch(MotorState &M, bool isM1) {
  uint32_t nowUnix=(uint32_t)getCurrentUnixTime();
  if (nowUnix==0||M.startUnix==0) return;
  if (M.appManualStopUntil>0&&nowUnix>=M.appManualStopUntil) M.appManualStopUntil=0;
  const String &url=isM1?urlM1:urlM2;
  if (!M.active&&M.sch_enabled&&!M.otTripped&&
      nowUnix>=M.startUnix&&nowUnix<M.stopUnix&&
      !(M.appManualStopUntil>0&&nowUnix<M.appManualStopUntil)) {
    sendToMaster(isM1?"M1:1":"M2:1"); M.active=true; M.state=1;
    updateTableDirect(url,1,0); updateAck(url,0);
  }
  if (M.active&&nowUnix>=M.stopUnix) {
    sendToMaster(isM1?"M1:0":"M2:0"); M.active=false; M.state=0;
    updateTableDirect(url,0,0); updateAck(url,0);
  }
}

void updateDisplay() {
  colonBlink=!colonBlink;
  if (rtcAvail&&rtc.isrunning()) {
    DateTime dt=rtc.now();
    dispObj.showNumberDecEx(dt.hour()*100+dt.minute(),colonBlink?0b01000000:0,true);
  }
}

void net_wifiReconnectIfNeeded() {
  if (WiFi.status()==WL_CONNECTED) return;
  unsigned long now=millis();
  if (now-lastWifiMs<30000UL) return;
  lastWifiMs=now; WiFi.disconnect(); WiFi.begin(WIFI_SSID,WIFI_PASS);
}

void setup() {
  Serial.begin(9600); delay(300);
  Wire.begin();
  if (rtc.begin()) {
    rtcAvail=true;
    if (!rtc.isrunning()) rtc.adjust(DateTime(F(__DATE__),F(__TIME__)));
  }
  dispObj.setBrightness(0x0a); dispObj.clear();
  buildUrls();
  configTime(0,0,"pool.ntp.org","time.nist.gov");
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASS);
  unsigned long t=millis();
  while (WiFi.status()!=WL_CONNECTED&&millis()-t<20000UL) delay(400);
  if (WiFi.status()==WL_CONNECTED) { net_login(); net_compareAndSyncTime(); }
}

void loop() {
  pollMasterSerial(); sendHeartbeatToMaster(); updateDisplay();
  unsigned long now=millis();
  if (now-lastExecMs<EXECUTION_INTERVAL) { delay(20); return; }
  lastExecMs=now;
  syncMasterStatusToCloud();
  checkSch(M1,true); checkSch(M2,false);
  net_wifiReconnectIfNeeded();
  if (WiFi.status()==WL_CONNECTED) {
    if (!login_status) net_login(); else checkTokenRefresh();
    if (login_status) {
      sendHeartbeatCloud(urlM1,M1.runtimeSecs);
      sendHeartbeatCloud(urlM2,M2.runtimeSecs);
      if (net_hasCloudChanged(DEVICE_ID_M1,lastChangeM1)) net_getTableData(urlM1,M1,true);
      if (net_hasCloudChanged(DEVICE_ID_M2,lastChangeM2)) net_getTableData(urlM2,M2,false);
    }
    net_compareAndSyncTime();
  }
  delay(20);
}
