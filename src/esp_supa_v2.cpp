#include <Arduino.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <time.h>
#include <TM1637Display.h>
#include <Wire.h>
#include "RTClib.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>

// ─────────────────────────── PIN MAP ────────────────────────
#define MOTOR_PIN      D7
#define CLK_PIN        D3
#define DIO_PIN        D4
#define OT_SENSOR_PIN  D8
#define BUTTON_PIN     D9   // active-LOW with INPUT_PULLUP


#define EEPROM_SIZE                 32
#define EEPROM_ADDR_MANUAL_UNTIL     0   // 4 bytes  uint32_t (0 = no block)
#define EEPROM_ADDR_OT_TRIPPED       4   // 1 byte   uint8_t  (1 = tripped)
#define EEPROM_ADDR_LAST_STATE1      5   // 1 byte   uint8_t  (0/1)
#define EEPROM_ADDR_MOTOR_ON_SINCE   6   // 4 bytes  uint32_t (0 = not running)

// ─────────────────────────── DEVICE IDENTITY ─────────────────
#define DEVICE_ID              1001UL
const char FIRMWARE_VERSION[] = "3.4.0";

// ─────────────────────────── TUNABLES ────────────────────────
#define OT_TRIP_COUNT               5
#define TOKEN_REFRESH_BEFORE_S      120
#define DRIFT_THRESHOLD_S           30
#define TIME_SYNC_INTERVAL_MS       (1UL * 60 * 60 * 1000)   // 1 hour
#define COMMAND_POLL_INTERVAL_MS    (10UL * 1000)            // read state_1/2
#define HEARTBEAT_INTERVAL_MS       (30UL * 1000)            // device_report
#define SCHEDULE_FETCH_INTERVAL_MS  (2UL * 60 * 1000)        // re-read device_seq/sch
#define WIFI_RETRY_INTERVAL_MS      (30UL * 1000)
#define MAX_SAFETY_RUNTIME_MIN      (30UL * 60 * 1000)   // hard cutoff regardless of schedule/app
#define MAX_SCHEDULES               8
#define HTTP_CONNECT_TIMEOUT_MS     3000    // bounds the TCP/TLS connect phase of every call
#define HTTP_RESPONSE_TIMEOUT_MS    5000    // bounds waiting for a response after connecting

// ─────────────────────────── CREDENTIALS (hardcoded) ─────────
const char WIFI_SSID[]      = "anupam";
const char WIFI_PASS[]      = "12345678";
const char USER_EMAIL[]     = "9630852741@gmail.com";
const char USER_PASS[]      = "123456";
const char SUPABASE_URL[]   = "https://pzwatyfdvltsvelppdew.supabase.co";
const char SUPABASE_KEY[]   = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InB6d2F0eWZkdmx0c3ZlbHBwZGV3Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODg1ODY4NzQsImV4cCI6MjEwNDE2Mjg3NH0.v1ilcWbF62gpnBvN4gBB6_qEETqie_mNabEa-K28oGw";


// Built once in setup()
String url_device_select;   // GET  state_1,state_2 (light poll)
String url_device_seq;      // GET  device_seq + sch join (schedules)
String url_rpc_command;     // POST set_device_command
String url_rpc_report;      // POST device_report

// ─────────────────────────── HARDWARE OBJECTS ────────────────
RTC_DS1307     rtc;
TM1637Display  dispObj(CLK_PIN, DIO_PIN);
bool           rtcAvail = false;

// ─────────────────────────── SCHEDULE MODEL ──────────────────
struct ScheduleEntry {
  int32_t  seqId        = -1;
  uint32_t startUnix     = 0;
  uint32_t stopUnix      = 0;
  bool     enabled       = false;
  bool     valid         = false;
};
ScheduleEntry schedules[MAX_SCHEDULES];
int           scheduleCount   = 0;
int           activeSchIdx    = -1;   // index into schedules[] currently driving the motor, -1 = none

// ─────────────────────────── AUTH STATE ──────────────────────
String        USER_TOKEN      = "";
bool          login_status    = false;
unsigned long tokenExpiresAt  = 0;   // millis() deadline

// ─────────────────────────── RUNTIME STATE ───────────────────
uint32_t      manualBlockUntil   = 0;      // schedule re-trigger suppressed until this unix time
volatile bool otTripped          = false;  // volatile: read directly inside buttonISR()
int           ot_sensorcount     = 0;
int           lastAppliedState1  = -1;     // -1 = unknown (forces first apply)
uint32_t      motorOnSinceUnix   = 0;      // for safety cutoff + display runtime
int           runtimeSecs        = 0;
bool          timeSynced         = false;
unsigned long lastSyncMs         = 0;
unsigned long lastCommandPollMs  = 0;
unsigned long lastHeartbeatMs    = 0;
unsigned long lastScheduleFetchMs= 0 - SCHEDULE_FETCH_INTERVAL_MS; // force fetch on first loop
unsigned long lastWifiAttemptMs  = 0;
unsigned long motorStartMs       = 0;
bool          colonBlink         = false;
String        pendingLastError   = "";     // sent on next heartbeat, then cleared

volatile bool buttonPressedFlag = false;
volatile unsigned long lastButtonIsrMs = 0;

bool lastCallFailedHard = false;   // set true the instant any HTTP call can't reach the server
bool internetAvailable = false;    // true only after a real internet check (or call) succeeds
unsigned long lastInternetCheckMs = 0;
#define INTERNET_CHECK_INTERVAL_MS (10UL * 1000)

void     buildUrls();
void     saveEeprom();
void     loadEeprom();
time_t   getCurrentUnixTime();
uint32_t hourMinuteSecToUnixUTC(uint8_t hour, uint8_t minute, int32_t offsetSec);

bool     getHttpdata(const char *url, String &out);
bool     httpPostJson(const char *url, const char *body, const char *bearerToken, String *out);

void     net_login();
void     checkTokenRefresh();
void     net_compareAndSyncTime();
void     net_fetchSchedules();
void     net_pollCommandState();
void     net_setDeviceCommand(int state1, int state2);
void     net_deviceReport();
void     net_wifiReconnectIfNeeded();
void     net_manageConnectivity();

void     applyMotorCommand(bool on, bool viaSchedule);
void     checkSchedules(uint32_t nowUnix);
void     checkSafetyCutoff(uint32_t nowUnix);
void     processOTSensor();
void     handleButtonPress();
void     handleButtonEvent();
void     syncButtonMotorState();
void     updateDisplay();
void     updateMotorRuntimeCounter();
void     printStatusLine();


// ───────────────────────────────────────────────────────────────
// buildUrls()
// Builds every REST/RPC URL the device calls, once, at boot.
// Keeping these as pre-built Strings avoids re-concatenating the
// same base URL on every single HTTP call later on.
// ───────────────────────────────────────────────────────────────
void buildUrls() {
  String base = String(SUPABASE_URL);
  url_device_select = base + "/rest/v1/device?device_id=eq." + String(DEVICE_ID) +
                       "&select=state_1,state_2";
  url_device_seq     = base + "/rest/v1/device_seq?device_id=eq." + String(DEVICE_ID) +
                       "&select=seq_id,duration,start_offset_sec,sch_enable,sch(start,duration)"
                       "&order=seq_id";
  url_rpc_command    = base + "/rest/v1/rpc/set_device_command";
  url_rpc_report     = base + "/rest/v1/rpc/report_device_state";
}

// ───────────────────────────────────────────────────────────────
// getCurrentUnixTime()
// Returns "now" as a unix timestamp. Prefers the hardware RTC
// (works even without WiFi); falls back to the ESP8266's internal
// clock (set via NTP in setup()) if there's no RTC.
// Returns 0 if neither source has a valid time yet.
// ───────────────────────────────────────────────────────────────
time_t getCurrentUnixTime() {
  if (rtcAvail && rtc.isrunning())
    return (time_t)rtc.now().unixtime();
  time_t t = time(nullptr);
  return (t > 1000000000UL) ? t : 0;
}

// ───────────────────────────────────────────────────────────────
// hourMinuteSecToUnixUTC()
// Takes a schedule's "start" time (hour:minute) plus a signed
// offset in seconds, and converts it into a full unix timestamp
// for *today*, based on the current RTC/NTP date. Used to turn
// the cloud's daily schedule rows into concrete start/stop times.
//
// FIX: mktime() returns (time_t)-1 on failure. Without a check,
// (uint32_t)(-1 + offsetSec) wraps around to a huge number near
// UINT32_MAX (e.g. 4294967130), which then gets treated as a
// "valid" far-future timestamp downstream - and if that garbage
// value ever ends up in manualBlockUntil (via a manual stop
// mid-schedule), it permanently blocks every future schedule
// since nowUnix will never reach it. Returning 0 instead makes
// net_fetchSchedules()'s existing validity check correctly reject
// this schedule instead of silently accepting corrupted data.
// ───────────────────────────────────────────────────────────────
uint32_t hourMinuteSecToUnixUTC(uint8_t hour, uint8_t minute, int32_t offsetSec) {
  time_t now = getCurrentUnixTime();
  if (now == 0) return 0;
  struct tm t;
  gmtime_r(&now, &t);
  t.tm_hour = hour;
  t.tm_min  = minute;
  t.tm_sec  = 0;
  time_t base = mktime(&t);
  if (base == (time_t)-1) return 0;   // mktime failed - don't return wraparound garbage
  return (uint32_t)(base + offsetSec);
}

// ───────────────────────────────────────────────────────────────
// saveEeprom()
// Persists the small set of values that must survive a reboot
// (manual-block window, OT-trip flag, last motor state, motor
// on-since time) to EEPROM. Only actually writes/commits when
// something changed, to avoid unnecessary flash wear.
// ───────────────────────────────────────────────────────────────
void saveEeprom() {
  bool dirty = false;
  uint32_t curManualUntil; EEPROM.get(EEPROM_ADDR_MANUAL_UNTIL, curManualUntil);
  if (curManualUntil != manualBlockUntil) {
    EEPROM.put(EEPROM_ADDR_MANUAL_UNTIL, manualBlockUntil);
    dirty = true;
  }
  uint8_t curOt; EEPROM.get(EEPROM_ADDR_OT_TRIPPED, curOt);
  uint8_t o = otTripped ? 1 : 0;
  if (curOt != o) {
    EEPROM.put(EEPROM_ADDR_OT_TRIPPED, o);
    dirty = true;
  }
  uint8_t curState1; EEPROM.get(EEPROM_ADDR_LAST_STATE1, curState1);
  uint8_t s1 = (lastAppliedState1 == 1) ? 1 : 0;
  if (curState1 != s1) {
    EEPROM.put(EEPROM_ADDR_LAST_STATE1, s1);
    dirty = true;
  }
  uint32_t curOnSince; EEPROM.get(EEPROM_ADDR_MOTOR_ON_SINCE, curOnSince);
  if (curOnSince != motorOnSinceUnix) {
    EEPROM.put(EEPROM_ADDR_MOTOR_ON_SINCE, motorOnSinceUnix);
    dirty = true;
  }
  if (dirty) {
    EEPROM.commit();
  }
}

// ───────────────────────────────────────────────────────────────
// loadEeprom()
// Restores saved state at boot. If the motor was ON when power
// was lost (and OT hasn't tripped), it re-energizes the motor
// immediately so the pump doesn't stay off through a reboot,
// then waits for the cloud to confirm/override that state.
//
// FIX: in addition to the existing 0xFFFFFFFF "blank EEPROM"
// check, also reject any manualBlockUntil that's absurdly far in
// the future (> year ~2096). This clears out the specific
// wraparound-garbage value (~4294967130, i.e. 0xFFFFFF9A) that a
// prior mktime() failure could have already written to EEPROM,
// which would otherwise permanently block every schedule forever
// even after this fix is flashed - the previous check only caught
// exactly 0xFFFFFFFF, not nearby wrapped values.
// ───────────────────────────────────────────────────────────────
void loadEeprom() {
  EEPROM.get(EEPROM_ADDR_MANUAL_UNTIL,   manualBlockUntil);
  uint8_t o, s1;
  EEPROM.get(EEPROM_ADDR_OT_TRIPPED,     o);
  EEPROM.get(EEPROM_ADDR_LAST_STATE1,    s1);
  EEPROM.get(EEPROM_ADDR_MOTOR_ON_SINCE, motorOnSinceUnix);

  otTripped = (o == 1);
  if (manualBlockUntil == 0xFFFFFFFF || manualBlockUntil > 4000000000UL) manualBlockUntil = 0;
  if (motorOnSinceUnix == 0xFFFFFFFF) motorOnSinceUnix = 0;

  if (s1 == 1 && !otTripped) {
    Serial.println(F("[BOOT] Resuming motor ON from EEPROM until cloud confirms"));
    digitalWrite(MOTOR_PIN, HIGH);
    motorStartMs      = millis();
    lastAppliedState1 = 1;
    if (motorOnSinceUnix == 0) {
      time_t nowT = getCurrentUnixTime();
      if (nowT > 1000000000UL) motorOnSinceUnix = (uint32_t)nowT;
    }
  } else {
    lastAppliedState1 = 0;
    motorOnSinceUnix  = 0;
  }
  Serial.printf("[EEPROM] manualBlockUntil=%u ot=%d lastState1=%d onSince=%u\n",
                manualBlockUntil, (int)otTripped, s1, motorOnSinceUnix);
}


// ───────────────────────────────────────────────────────────────
// getHttpdata()
// Generic authenticated GET helper used by every "read" call
// (poll command state, fetch schedules). Bails out instantly if
// WiFi or internet isn't confirmed up — it never attempts a real
// connection in that case. If the connection genuinely fails
// (code < 0, e.g. can't reach the server even though WiFi is
// associated), it immediately marks internetAvailable = false so
// nothing else queues up behind another slow timeout this loop.
// ───────────────────────────────────────────────────────────────
bool getHttpdata(const char *url, String &out) {
  if (WiFi.status() != WL_CONNECTED || !internetAvailable) return false;

  WiFiClientSecure cl;
  cl.setInsecure();
  cl.setTimeout(HTTP_CONNECT_TIMEOUT_MS);   // bounds the TCP/TLS connect stage
  HTTPClient https;
  https.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  if (!https.begin(cl, url)) return false;
  https.addHeader(F("apikey"),        SUPABASE_KEY);
  https.addHeader(F("Authorization"), "Bearer " + USER_TOKEN);
  https.addHeader(F("Content-Type"),  F("application/json"));
  int code = https.GET();
  if (code < 0) {
    lastCallFailedHard = true;
    internetAvailable  = false;   // don't wait for the next scheduled check - drop it now
  }
  if (code == 200) out = https.getString();
  https.end();
  Serial.printf("[HTTP] GET %d %s\n", code, url);
  return (code == 200);
}

// ───────────────────────────────────────────────────────────────
// httpPostJson()
// Generic authenticated POST helper used by every "write" call
// (set_device_command, report_device_state). Same guards as
// getHttpdata(): skips instantly with no WiFi/internet, and on a
// hard failure (code < 0) immediately drops internetAvailable so
// a second call fired right after this one (e.g. applyMotorCommand
// calling net_deviceReport() straight after net_setDeviceCommand())
// doesn't also sit through a full timeout.
// ───────────────────────────────────────────────────────────────
bool httpPostJson(const char *url, const char *body, const char *bearerToken, String *out) {
  if (WiFi.status() != WL_CONNECTED || !internetAvailable) return false;

  WiFiClientSecure cl;
  cl.setInsecure();
  cl.setTimeout(HTTP_CONNECT_TIMEOUT_MS);   // bounds the TCP/TLS connect stage
  HTTPClient https;
  https.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  if (!https.begin(cl, url)) return false;
  https.addHeader(F("apikey"), SUPABASE_KEY);
  String bearer = bearerToken ? String(bearerToken) : String(SUPABASE_KEY);
  https.addHeader(F("Authorization"), "Bearer " + bearer);
  https.addHeader(F("Content-Type"),  F("application/json"));
  https.addHeader(F("Prefer"),        F("return=minimal"));
  int code = https.POST(body);
  if (code < 0) {
    lastCallFailedHard = true;
    internetAvailable  = false;   // don't wait for the next scheduled check - drop it now
  }
  if (out && code >= 200 && code < 300) *out = https.getString();
  https.end();
  Serial.printf("[HTTP] POST %d %s\n", code, url);
  return (code >= 200 && code < 300);
}


// ───────────────────────────────────────────────────────────────
// net_login()
// Logs in to Supabase with the hardcoded email/password and
// stores the access token + its expiry deadline. Skipped entirely
// if WiFi/internet isn't confirmed up. Called at boot, whenever
// login_status is false and internet is up, and whenever the
// token is about to expire (see checkTokenRefresh()).
// ───────────────────────────────────────────────────────────────
void net_login() {
  if (WiFi.status() != WL_CONNECTED || !internetAvailable) return;

  String authUrl = String(SUPABASE_URL) + "/auth/v1/token?grant_type=password";
  WiFiClientSecure cl;
  cl.setInsecure();
  cl.setTimeout(HTTP_CONNECT_TIMEOUT_MS);
  HTTPClient https;
  https.setTimeout(8000);
  if (!https.begin(cl, authUrl.c_str())) return;
  https.addHeader(F("apikey"),       SUPABASE_KEY);
  https.addHeader(F("Content-Type"), F("application/json"));
  String body = "{\"email\":\"" + String(USER_EMAIL) +
                "\",\"password\":\"" + String(USER_PASS) + "\"}";
  int code = https.POST(body);
  Serial.printf("[AUTH] Login HTTP %d\n", code);
  if (code < 0) internetAvailable = false;   // couldn't even reach the auth server
  if (code == 200) {
    StaticJsonDocument<1024> doc;
    if (!deserializeJson(doc, https.getString()) &&
        doc["access_token"].is<const char *>()) {
      USER_TOKEN     = doc["access_token"].as<String>();
      unsigned long expiresIn = doc["expires_in"] | 3600UL;
      tokenExpiresAt = millis() + ((expiresIn - TOKEN_REFRESH_BEFORE_S) * 1000UL);
      login_status   = true;
      Serial.printf("[AUTH] OK - token expires in %lu s\n", expiresIn);
    }
  } else {
    login_status = false;
    Serial.println(F("[AUTH] Login failed"));
  }
  https.end();
}

// ───────────────────────────────────────────────────────────────
// net_checkInternet()
// Lightweight, unauthenticated check: is there an actual path to
// the internet, not just an associated WiFi AP? Hits GoTrue's
// /health endpoint (needs no login) so it never depends on
// login_status. Runs on a timer (every INTERNET_CHECK_INTERVAL_MS)
// from net_manageConnectivity(), with a short (3s) timeout so it
// never blocks the rest of loop() for long. This is the single
// source of truth for the internetAvailable flag - individual
// HTTP calls can also drop it early on a hard failure, but only
// this function is allowed to set it back to true.
//
// If WiFi is associated to the AP but this check fails (router's
// WAN/uplink is down, captive portal, etc.), WiFi.status() would
// otherwise stay WL_CONNECTED forever and net_manageConnectivity()
// would just keep polling this same failed check every 10s with
// nothing ever recovering. To avoid that dead state, we force a
// WiFi.disconnect() here: that flips WiFi.status() away from
// WL_CONNECTED, which hands control straight back to the normal
// non-blocking WiFi watchdog (net_wifiReconnectIfNeeded()) so the
// device retries the *whole* WiFi connection on its usual
// WIFI_RETRY_INTERVAL_MS cadence, exactly like a real WiFi drop.
// Nothing else in this function blocks - WiFi.disconnect() is
// non-blocking on ESP8266.
// ───────────────────────────────────────────────────────────────
bool net_checkInternet() {
  if (WiFi.status() != WL_CONNECTED) { internetAvailable = false; return false; }
  WiFiClientSecure cl;
  cl.setInsecure();
  cl.setTimeout(HTTP_CONNECT_TIMEOUT_MS);
  HTTPClient https;
  https.setTimeout(HTTP_CONNECT_TIMEOUT_MS);
  String url = String(SUPABASE_URL) + "/auth/v1/health";
  if (!https.begin(cl, url.c_str())) { internetAvailable = false; return false; }
  https.addHeader(F("apikey"), SUPABASE_KEY);
  int code = https.GET();
  https.end();
  internetAvailable = (code > 0);   // any real HTTP response = internet reachable
  Serial.printf("[NET] Internet check: %s (code %d)\n", internetAvailable ? "UP" : "DOWN", code);

  if (!internetAvailable) {
    // WiFi is associated but there's no real internet path - drop WiFi so
    // the watchdog retries the connection instead of idling forever.
    Serial.println(F("[NET] No internet on this WiFi link - disconnecting WiFi to force a fresh reconnect"));
    WiFi.disconnect();
    login_status = false;   // any token we had is now stale/unusable anyway
  }

  return internetAvailable;
}

// ───────────────────────────────────────────────────────────────
// checkTokenRefresh()
// Called only while login_status is true. Once the token's
// deadline (millis()) passes, marks us logged-out and immediately
// tries net_login() again to get a fresh token.
// ───────────────────────────────────────────────────────────────
void checkTokenRefresh() {
  if (!login_status) return;
  if (millis() >= tokenExpiresAt) {
    Serial.println(F("[AUTH] Token expiring - re-logging in"));
    login_status = false;
    net_login();
  }
}


// ───────────────────────────────────────────────────────────────
// net_compareAndSyncTime()
// Once an hour (TIME_SYNC_INTERVAL_MS), fetches real time from an
// external time API and, if the hardware RTC has drifted more
// than DRIFT_THRESHOLD_S seconds, corrects it. Skipped entirely
// without a confirmed WiFi + internet path.
// ───────────────────────────────────────────────────────────────
void net_compareAndSyncTime() {
  if (timeSynced && (millis() - lastSyncMs < TIME_SYNC_INTERVAL_MS)) return;
  if (WiFi.status() != WL_CONNECTED || !internetAvailable) return;

  WiFiClientSecure cl;
  cl.setInsecure();
  cl.setTimeout(HTTP_CONNECT_TIMEOUT_MS);
  HTTPClient https;
  https.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  if (!https.begin(cl, "https://api.skyiottech.com/time")) return;
  int    code = https.GET();
  String body = (code == 200) ? https.getString() : "";
  https.end();
  if (code < 0) internetAvailable = false;   // couldn't reach the time server either

  if (code != 200 || body.isEmpty()) {
    Serial.println(F("[SYNC] Failed to get internet time"));
    return;
  }
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) return;

  time_t internetTime = (time_t)doc["unix_time"].as<unsigned long>();

  if (rtcAvail && rtc.isrunning()) {
    long drift = abs((long)(internetTime - (long)rtc.now().unixtime()));
    Serial.printf("[SYNC] Drift: %ld s\n", drift);
    if (drift > DRIFT_THRESHOLD_S) {
      rtc.adjust(DateTime((uint32_t)internetTime));
      Serial.println(F("[SYNC] RTC updated"));
    }
  }
  timeSynced = true;
  lastSyncMs = millis();
}


// ───────────────────────────────────────────────────────────────
// net_pollCommandState()
// Reads state_1 from the cloud "device" row and applies it locally
// if it differs from what we last applied (handles the case where
// the phone app turned the pump on/off). Skipped entirely without
// a confirmed WiFi + internet path, and without being logged in.
// ───────────────────────────────────────────────────────────────
void net_pollCommandState() {
  if (!login_status || WiFi.status() != WL_CONNECTED || !internetAvailable) return;

  String resp;
  if (!getHttpdata(url_device_select.c_str(), resp)) return;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, resp)) {
    Serial.println(F("[POLL] JSON parse fail"));
    return;
  }
  if (!doc.is<JsonArray>() || doc.size() == 0) return;

  int cloudState1 = doc[0]["state_1"] | 0;

  if (lastAppliedState1 == -1) {

    bool wantOn = (cloudState1 == 1) && !otTripped;
    if (wantOn != (digitalRead(MOTOR_PIN) == HIGH)) {
      applyMotorCommand(wantOn, false);
    }
    saveEeprom();
    return;
  }

  if (cloudState1 != lastAppliedState1) {
    Serial.printf("[POLL] state_1 changed %d -> %d\n", lastAppliedState1, cloudState1);
    lastAppliedState1 = cloudState1;

    if (cloudState1 == 1 && !otTripped) {
      applyMotorCommand(true, false);
    } else if (cloudState1 == 0) {
  

      if (activeSchIdx >= 0) {
        manualBlockUntil = schedules[activeSchIdx].stopUnix;
        Serial.printf("[POLL] App stop mid-schedule - blocked until %u\n", manualBlockUntil);
        activeSchIdx = -1;
      }
      applyMotorCommand(false, false);
    }
    saveEeprom();
  }
}

// ───────────────────────────────────────────────────────────────
// net_fetchSchedules()
// Reads the device_seq -> sch join from the cloud and recomputes
// each schedule's today's start/stop unix timestamps into the
// local schedules[] array. Skipped entirely without a confirmed
// WiFi + internet path, and without being logged in.
// ───────────────────────────────────────────────────────────────
void net_fetchSchedules() {
  if (!login_status || WiFi.status() != WL_CONNECTED || !internetAvailable) return;

  String resp;
  if (!getHttpdata(url_device_seq.c_str(), resp)) return;

  DynamicJsonDocument doc(3072);
  DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    Serial.println(F("[SCH] JSON parse fail"));
    return;
  }
  if (!doc.is<JsonArray>()) return;

  int n = 0;
  for (JsonObject row : doc.as<JsonArray>()) {
    if (n >= MAX_SCHEDULES) break;

    JsonObject schObj = row["sch"];
    if (schObj.isNull()) continue; // row filtered out by RLS join / deleted parent

    const char *startStr = schObj["start"] | "00:00:00";
    uint8_t h = 0, m = 0;
    if (strlen(startStr) >= 5) {
      h = (startStr[0] - '0') * 10 + (startStr[1] - '0');
      m = (startStr[3] - '0') * 10 + (startStr[4] - '0');
    }
    int32_t  offsetSec = row["start_offset_sec"] | 0;
    uint32_t durationMin = row["duration"] | (schObj["duration"] | 10);
    bool     enabled     = row["sch_enable"] | false;

    uint32_t startUnix = hourMinuteSecToUnixUTC(h, m, offsetSec);
    uint32_t stopUnix  = startUnix + (durationMin * 60UL);

    schedules[n].seqId     = row["seq_id"] | -1;
    schedules[n].startUnix = startUnix;
    schedules[n].stopUnix  = stopUnix;
    schedules[n].enabled   = enabled;
    schedules[n].valid     = (startUnix > 1000000000UL) && (stopUnix > startUnix);
    n++;
  }
  scheduleCount = n;
  Serial.printf("[SCH] Loaded %d schedule(s)\n", scheduleCount);

  if (activeSchIdx >= scheduleCount) activeSchIdx = -1;
}


// ───────────────────────────────────────────────────────────────
// net_setDeviceCommand()
// Tells the cloud what state_1/state_2 the device is now driving
// (e.g. after a button press or schedule start/stop), via the
// set_device_command RPC. Skipped instantly, with a clear log
// line, whenever not logged in or WiFi/internet isn't confirmed.
// ───────────────────────────────────────────────────────────────
void net_setDeviceCommand(int state1, int state2) {
if (!login_status || WiFi.status() != WL_CONNECTED || !internetAvailable) {
  Serial.println(F("[CMD] Skipped - not logged in / no WiFi"));
  return;
}
  String body = "{\"p_device_id\":" + String(DEVICE_ID) +
                ",\"p_state_1\":" + String(state1) +
                ",\"p_state_2\":" + String(state2) + "}";
  if (httpPostJson(url_rpc_command.c_str(), body.c_str(), USER_TOKEN.c_str(), nullptr)) {
    lastAppliedState1 = state1;
  } else {
    Serial.println(F("[CMD] set_device_command failed"));
  }
}


// ───────────────────────────────────────────────────────────────
// net_deviceReport()
// Sends a heartbeat/status report to the cloud: current motor
// state, "online" marker, firmware version, MAC address, and any
// pending error message. Skipped instantly, with a clear log
// line, whenever not logged in or WiFi/internet isn't confirmed.
// ───────────────────────────────────────────────────────────────
void net_deviceReport() {
 if (!login_status || WiFi.status() != WL_CONNECTED || !internetAvailable) {
  Serial.println(F("[CMD] Skipped - not logged in / no WiFi"));
  return;
}

  int deviceState = (digitalRead(MOTOR_PIN) == HIGH) ? 1 : 0;
  String mac = WiFi.macAddress();

  String body = "{\"p_device_id\":" + String(DEVICE_ID) +
                ",\"p_device_state\":" + String(deviceState) +
                ",\"p_online\":10" +
                ",\"p_firmware_version\":\"" + String(FIRMWARE_VERSION) + "\"" +
                ",\"p_mac_address\":\"" + mac + "\"";
  if (pendingLastError.length() > 0) {
    body += ",\"p_last_error\":\"" + pendingLastError + "\"";
  } else {
    body += ",\"p_last_error\":null";
  }
  body += "}";

  if (httpPostJson(url_rpc_report.c_str(), body.c_str(), USER_TOKEN.c_str(), nullptr)) {
    pendingLastError = ""; // delivered, clear so we don't keep re-sending it forever
  } else {
    Serial.println(F("[REPORT] report_device_state failed"));
  }
}

// ───────────────────────────────────────────────────────────────
// net_wifiReconnectIfNeeded()
// Non-blocking WiFi watchdog: if we're not connected, and it's
// been at least WIFI_RETRY_INTERVAL_MS since the last attempt,
// kick off a fresh (async) WiFi.begin(). Never uses delay(). This
// is the ONLY place that re-initiates a WiFi connection - whether
// we got here because WiFi genuinely dropped, or because
// net_checkInternet() forced a disconnect after finding no real
// internet path, the recovery path is identical.
// ───────────────────────────────────────────────────────────────
void net_wifiReconnectIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastWifiAttemptMs < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiAttemptMs = now;
  Serial.println(F("[WIFI] Reconnecting (non-blocking)"));
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// ───────────────────────────────────────────────────────────────
// net_manageConnectivity()
// Single entry point for everything network-related, called once
// per loop(). The rule is simple and absolute: no confirmed
// internet path = NOTHING network-related runs, full stop, no
// exceptions, no background timers left ticking.
//
//   1. If WiFi isn't connected, force internetAvailable and
//      login_status false and return immediately. This is the
//      only "recovery" path left alive - net_wifiReconnectIfNeeded()
//      above it, on its own WIFI_RETRY_INTERVAL_MS timer. Nothing
//      else in this function - no login, no polling, no heartbeat,
//      no schedule fetch, no time sync - runs while this is true.
//      This also covers the moment right after net_checkInternet()
//      has forced a WiFi.disconnect(): on the very next call here,
//      WiFi.status() is no longer WL_CONNECTED, so everything below
//      stops immediately, with no lingering retries of any kind.
//   2. Only once WiFi is actually connected do we even look at the
//      internet: on a single INTERNET_CHECK_INTERVAL_MS timer, we
//      verify there's a real path (not just AP association) and,
//      in the same tick, attempt login if we're not already logged
//      in. There is no separate faster login-retry timer anymore -
//      login is only ever attempted right after a fresh internet
//      check confirms it's actually worth trying.
//   3. If internet isn't confirmed up (either the check just failed,
//      or we haven't run it yet), we stop here too - same as step 1.
//   4. Only with internet confirmed up AND logged in do token
//      refresh, schedule-fetch, command-poll, heartbeat, and time
//      sync run, each on its own independent timer.
// Every net_* function called from here still has its own internal
// WiFi+internet guard too, so this is belt-and-suspenders: nothing
// here can block for long even if internet drops mid-call, because
// the HTTP helpers drop internetAvailable immediately on a hard
// failure instead of waiting for the next timed check.
// ───────────────────────────────────────────────────────────────
void net_manageConnectivity() {
  net_wifiReconnectIfNeeded();

  if (WiFi.status() != WL_CONNECTED) {
    internetAvailable = false;   // no WiFi -> no internet, full stop
    login_status      = false;   // any session is dead the moment the link is dead
    return;                      // absolutely nothing else in this function runs
  }

  unsigned long now = millis();

  // Single timer for "is the internet actually up" + "try to log in if so".
  // No separate faster retry loop for login - it only ever fires right
  // after a fresh internet check has just confirmed there's a real path.
  if (now - lastInternetCheckMs >= INTERNET_CHECK_INTERVAL_MS) {
    lastInternetCheckMs = now;
    net_checkInternet();   // may disconnect WiFi itself if there's no real internet path

    if (internetAvailable && !login_status) {
      net_login();
    }
  }

  if (!internetAvailable) return;   // no confirmed internet - stop here, nothing below runs

  if (login_status) {
    checkTokenRefresh();
  }

  lastCallFailedHard = false;

  if (login_status && (now - lastScheduleFetchMs >= SCHEDULE_FETCH_INTERVAL_MS)) {
    lastScheduleFetchMs = now;
    net_fetchSchedules();
  }

  if (login_status && (now - lastCommandPollMs >= COMMAND_POLL_INTERVAL_MS)) {
    lastCommandPollMs = now;
    net_pollCommandState();
  }

  if (login_status && (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS)) {
    lastHeartbeatMs = now;
    net_deviceReport();
  }

  net_compareAndSyncTime();
}


// ───────────────────────────────────────────────────────────────
// applyMotorCommand()
// The single place that actually toggles the MOTOR_PIN. Records
// motorStartMs/motorOnSinceUnix (for the on-screen runtime and the
// safety cutoff), persists state to EEPROM, and immediately fires
// a device report so the cloud sees the new state without waiting
// for the next heartbeat. Does nothing if the motor is already in
// the requested state.
// ───────────────────────────────────────────────────────────────
void applyMotorCommand(bool on, bool viaSchedule) {
  bool currentlyOn = (digitalRead(MOTOR_PIN) == HIGH);
  if (on == currentlyOn) return;

  digitalWrite(MOTOR_PIN, on ? HIGH : LOW);
  if (on) {
    motorStartMs     = millis();
    runtimeSecs      = 0;
    time_t nowT = getCurrentUnixTime();
    motorOnSinceUnix = (nowT > 1000000000UL) ? (uint32_t)nowT : 0;
    Serial.println(viaSchedule ? F("[MOTOR] ON (schedule)") : F("[MOTOR] ON"));
  } else {
    runtimeSecs      = 0;
    motorOnSinceUnix = 0;
    Serial.println(viaSchedule ? F("[MOTOR] OFF (schedule)") : F("[MOTOR] OFF"));
  }
  saveEeprom();
  net_deviceReport(); // push actual state immediately; no-ops cleanly if WiFi/internet is down
}

// ───────────────────────────────────────────────────────────────
// checkSchedules()
// Runs every loop() with the current unix time. Clears an expired
// manual-stop block, stops the motor if the active schedule's
// window has ended, and otherwise starts the motor if we've
// entered a valid, enabled schedule's window (and nothing else -
// button, app, OT trip, manual block - is already controlling it).
// ───────────────────────────────────────────────────────────────
void checkSchedules(uint32_t nowUnix) {
  // Clear an expired manual block.
  if (manualBlockUntil > 0 && nowUnix >= manualBlockUntil) {
    Serial.println(F("[SCH] Manual-stop window expired - block cleared"));
    manualBlockUntil = 0;
    saveEeprom();
  }

  // Stop condition for whichever schedule is currently active.
  if (activeSchIdx >= 0) {
    if (nowUnix >= schedules[activeSchIdx].stopUnix) {
      Serial.printf("[SCH] STOP seq_id=%d\n", schedules[activeSchIdx].seqId);
      net_setDeviceCommand(0, 0);
      applyMotorCommand(false, true);
      activeSchIdx = -1;
    }
    return; // don't evaluate start conditions while one schedule is running
  }

  if (otTripped) return;
  if (manualBlockUntil > 0 && nowUnix < manualBlockUntil) return;
  if (digitalRead(MOTOR_PIN) == HIGH) return; // already on for some other reason (e.g. button)

  for (int i = 0; i < scheduleCount; i++) {
    if (!schedules[i].valid || !schedules[i].enabled) continue;
    if (nowUnix >= schedules[i].startUnix && nowUnix < schedules[i].stopUnix) {
      Serial.printf("[SCH] START seq_id=%d\n", schedules[i].seqId);
      net_setDeviceCommand(1, 0);
      applyMotorCommand(true, true);
      activeSchIdx = i;
      break;
    }
  }
}

// ───────────────────────────────────────────────────────────────
// checkSafetyCutoff()
// Independent hard cutoff that protects the pump/well even if the
// app or cloud never sends an OFF command: if the motor has been
// on for longer than MAX_SAFETY_RUNTIME_MIN, force it off and
// clear any manual block so schedules can re-evaluate normally.
// (Currently not called from loop() - left available if needed.)
// ───────────────────────────────────────────────────────────────
void checkSafetyCutoff(uint32_t nowUnix) {
  if (digitalRead(MOTOR_PIN) != HIGH) return;
  if (motorOnSinceUnix == 0) return; // no RTC / unknown start time, can't evaluate
  uint32_t elapsedMin = (nowUnix - motorOnSinceUnix) / 60UL;
  if (elapsedMin >= (uint32_t)MAX_SAFETY_RUNTIME_MIN) {
    Serial.println(F("[SAFETY] Max runtime exceeded - forcing OFF"));
    pendingLastError = "Safety runtime cutoff (" + String(MAX_SAFETY_RUNTIME_MIN) + " min)";
    net_setDeviceCommand(0, 0);
    applyMotorCommand(false, false);
    activeSchIdx = -1;
    manualBlockUntil = 0; // let schedules re-evaluate normally next window
  }
}

// ───────────────────────────────────────────────────────────────
// processOTSensor()
// Debounced over-temperature trip logic: counts consecutive LOW
// readings on OT_SENSOR_PIN while the motor is running, and once
// OT_TRIP_COUNT is reached, forces the motor off, sets otTripped,
// and shows "ERRO" on the display. Cleared by a button press.
// (Currently not called from loop() - left available if needed.)
// ───────────────────────────────────────────────────────────────
void processOTSensor() {
  if (digitalRead(MOTOR_PIN) == LOW) {
    ot_sensorcount = 0;
    return;
  }
  if (digitalRead(OT_SENSOR_PIN) == LOW) {
    ot_sensorcount++;
    Serial.printf("[OT] Count: %d/%d\n", ot_sensorcount, OT_TRIP_COUNT);
    if (ot_sensorcount >= OT_TRIP_COUNT) {
      Serial.println(F("[OT] TRIP - Motor OFF"));
      otTripped = true;
      pendingLastError = "Overtemperature trip";
      net_setDeviceCommand(0, 0);
      applyMotorCommand(false, false);
      activeSchIdx      = -1;
      manualBlockUntil  = 0;
      ot_sensorcount    = 0;
      saveEeprom();
      // Show "ERRO" on display
      const uint8_t s[4] = { 0x79, 0x50, 0x50, 0x06 };
      dispObj.setSegments(s);
    }
  } else {
    ot_sensorcount = 0;
  }
}

// ───────────────────────────────────────────────────────────────
// syncButtonMotorState()
// buttonISR() has ALREADY flipped MOTOR_PIN directly, the instant
// the button was pressed - the pump never waits on this function.
// This just catches up everything that isn't safe or fast enough
// to do inside an ISR (RTC/I2C read for motorOnSinceUnix, manual-
// block math, EEPROM write, cloud notify) - none of which need to
// be instant, so it's fine if this runs a moment late (e.g. right
// after a blocking HTTP call elsewhere finally returns).
// ───────────────────────────────────────────────────────────────
void syncButtonMotorState() {
  bool nowOn = (digitalRead(MOTOR_PIN) == HIGH);   // reflects the ISR's toggle
  runtimeSecs = 0;

  if (nowOn) {
    motorStartMs = millis();
    time_t nowT = getCurrentUnixTime();
    motorOnSinceUnix = (nowT > 1000000000UL) ? (uint32_t)nowT : 0;
    net_setDeviceCommand(1, 0);
    Serial.println(F("[MOTOR] ON"));
  } else {
    if (activeSchIdx >= 0) {
      manualBlockUntil = schedules[activeSchIdx].stopUnix;
      Serial.printf("[BTN] Manual stop mid-schedule - blocked until %u\n", manualBlockUntil);
      activeSchIdx = -1;
    }
    motorOnSinceUnix = 0;
    net_setDeviceCommand(0, 0);
    Serial.println(F("[MOTOR] OFF"));
  }
  saveEeprom();
  net_deviceReport(); // push actual state immediately; no-ops cleanly if WiFi/internet is down
}

// ───────────────────────────────────────────────────────────────
// handleButtonPress()
// Local manual control. If OT-tripped, a press just clears the
// trip and reports it - no motor action, since the ISR itself
// refuses to toggle the pin while otTripped is set (see
// buttonISR()). Otherwise, the pump has already reacted (the ISR
// flipped MOTOR_PIN before this function ever runs); this only
// finishes the bookkeeping via syncButtonMotorState().
// ───────────────────────────────────────────────────────────────
void handleButtonPress() {
  Serial.println(F("[BTN] Press"));

  if (otTripped) {
    otTripped = false;
    pendingLastError = ""; // clears last_error on next report
    saveEeprom();
    dispObj.clear();
    net_deviceReport();
    Serial.println(F("[BTN] OT reset"));
    return;
  }

  syncButtonMotorState();
}

// ───────────────────────────────────────────────────────────────
// handleButtonEvent()
// Called once per loop(). Clears the ISR flag and dispatches to
// handleButtonPress() only when the button was actually pressed
// since the last check - keeps loop() itself free of the flag
// bookkeeping.
// ───────────────────────────────────────────────────────────────
void handleButtonEvent() {
  if (!buttonPressedFlag) return;
  buttonPressedFlag = false;
  handleButtonPress();
}

// ───────────────────────────────────────────────────────────────
// updateDisplay()
// Drives the TM1637: while the motor is running, shows elapsed
// MM:SS runtime; otherwise shows the current HH:MM from the RTC
// with a blinking colon. Does nothing if there's no running RTC
// and the motor is off (nothing sensible to show).
// ───────────────────────────────────────────────────────────────
void updateDisplay() {
  colonBlink = !colonBlink;
  if (digitalRead(MOTOR_PIN) == HIGH) {
    int mm = runtimeSecs / 60, ss = runtimeSecs % 60;
    dispObj.showNumberDecEx(mm * 100 + ss, 0b01000000, true);
  } else if (rtcAvail && rtc.isrunning()) {
    DateTime dt = rtc.now();
    dispObj.showNumberDecEx(dt.hour() * 100 + dt.minute(),
                            colonBlink ? 0b01000000 : 0, true);
  }
}

// ───────────────────────────────────────────────────────────────
// updateMotorRuntimeCounter()
// Called once per loop(). While the motor is on, recomputes
// runtimeSecs from motorStartMs so updateDisplay() always shows a
// fresh elapsed time.
// ───────────────────────────────────────────────────────────────
void updateMotorRuntimeCounter() {
  if (digitalRead(MOTOR_PIN) == HIGH) {
    runtimeSecs = (int)((millis() - motorStartMs) / 1000UL);
  }
}

// ───────────────────────────────────────────────────────────────
// printStatusLine()
// One compact debug line per loop() showing the current RTC time
// and manual-block state, only when the RTC is present and
// running. Purely cosmetic/diagnostic - safe to remove.
// ───────────────────────────────────────────────────────────────
void printStatusLine() {
if (rtcAvail && rtc.isrunning()) {
    DateTime dt = rtc.now();
    Serial.printf("[MAIN] Time:%02d:%02d Motor:%s Runtime:%ds OT:%s WiFi:%s ManualUntil:%u\n",
                  dt.hour(), dt.minute(),
                  digitalRead(MOTOR_PIN) ? "ON" : "OFF",
                  runtimeSecs,
                  otTripped ? "TRIP" : "ok",
                  WiFi.status() == WL_CONNECTED ? "OK" : "X",
                  manualBlockUntil);
  }

}

// ───────────────────────────────────────────────────────────────
// buttonISR()
// Hardware interrupt on BUTTON_PIN (FALLING = press, since it's
// active-LOW with INPUT_PULLUP). Debounced to 200ms in software.
//
// The physical relay is flipped RIGHT HERE, synchronously, the
// instant the press is detected - this is the one piece of work
// that must never wait on loop(), WiFi, or a blocking HTTP call.
// It's safe to do in an ISR because it's just a digitalRead() +
// digitalWrite() on a plain GPIO - no I2C/RTC access, no Strings,
// no network calls, nothing that can block or allocate.
//
// Everything else (cloud notify, EEPROM save, manual-block
// bookkeeping) is NOT time-critical, so it's left to
// handleButtonEvent() -> syncButtonMotorState() in the main loop,
// which may run a little late if loop() happens to be stuck in a
// blocking HTTP call at that moment - but by then the pump has
// already reacted, so the user never perceives that lag.
// ───────────────────────────────────────────────────────────────
ICACHE_RAM_ATTR void buttonISR() {
  unsigned long now = millis();
  if (now - lastButtonIsrMs > 200) {
    lastButtonIsrMs = now;
    if (!otTripped) {
      bool currentlyOn = (digitalRead(MOTOR_PIN) == HIGH);
      digitalWrite(MOTOR_PIN, currentlyOn ? LOW : HIGH);   // instant, non-blocking pump response
    }
    buttonPressedFlag = true;
  }
}


// ───────────────────────────────────────────────────────────────
// setup()
// One-time init: serial, EEPROM, RTC, motor/button/OT pins,
// display, URL strings, saved state, NTP config, and a blocking
// WiFi connect attempt (only place in the whole sketch allowed to
// block, since nothing else can run usefully before boot finishes
// anyway). If WiFi connects, does one full network pass (internets
// check, login, time sync, schedule fetch, initial report) before
// handing off to loop().
// ───────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\n=== Pump Controller ESP8266 v3.4 (supabase_all_tables schema) ==="));

  EEPROM.begin(EEPROM_SIZE);

  Wire.begin();   // default SDA=D2, SCL=D1 on ESP8266
  if (rtc.begin()) {
    rtcAvail = true;
    Serial.println(F("[RTC] DS1307 found"));
    if (!rtc.isrunning())
      Serial.println(F("[RTC] Not running - adjust time via sync"));
  } else {
    Serial.println(F("[RTC] Not found - using NTP fallback"));
  }

  pinMode(MOTOR_PIN,     OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(BUTTON_PIN,    INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
  pinMode(OT_SENSOR_PIN, INPUT_PULLUP);

  dispObj.setBrightness(0x0a);
  dispObj.clear();

  buildUrls();
  loadEeprom();

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("[WIFI] Connecting"));
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) {
    delay(400);
    Serial.print(F("."));
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Connected - IP: %s\n", WiFi.localIP().toString().c_str());
    net_checkInternet();      // establish internetAvailable before using it below
    net_login();
    net_compareAndSyncTime();
    net_fetchSchedules();
    lastScheduleFetchMs = millis();
    net_deviceReport();
  } else {
    Serial.println(F("\n[WIFI] Failed - will retry in loop"));
  }

  Serial.println(F("[BOOT] Setup done"));
}

// ───────────────────────────────────────────────────────────────
// loop()
// Main cycle, runs roughly every 100ms (see the trailing delay).
// Deliberately kept as a short, readable list of function calls -
// all the actual logic lives in the named functions above:
//   1. handleButtonEvent()          - manual on/off, OT reset
//   2. updateDisplay()               - refresh the TM1637
//   3. checkSchedules()              - auto start/stop by schedule
//   4. updateMotorRuntimeCounter()   - keep runtimeSecs current
//   5. net_manageConnectivity()      - all WiFi/internet/cloud I/O
//   6. printStatusLine()             - one debug line
// Nothing here ever calls delay() except the fixed 100ms loop
// pace at the very end - all network waits are bounded inside the
// HTTP helpers, so a dead internet connection can't freeze button
// presses, schedules, or the display for more than one HTTP
// timeout's worth of time.
// ───────────────────────────────────────────────────────────────
void loop() {
  handleButtonEvent();
  updateDisplay();

  time_t t = getCurrentUnixTime();
  if (t > 1000000000UL) {
    checkSchedules((uint32_t)t);
  }

  updateMotorRuntimeCounter();
  net_manageConnectivity();
  printStatusLine();

  delay(100);
}
