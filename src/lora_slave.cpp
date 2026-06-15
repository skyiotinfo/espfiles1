#include <Arduino.h>
#include <LoRa.h>
#include <SPI.h>
#include <EEPROM.h>

#define MOTOR_PIN      D2  
#define OT_SENSOR_PIN  D1   
#define MANUAL_BUTTON  D9   

#define LORA_NSS    15   // GPIO15 = D8
#define LORA_RST    16   // GPIO16 = D0
#define LORA_DIO0    2   // GPIO2  = D4

#define LORA_NETWORK_ID   "2015"
#define LORA_DEVICE_ID    "10"
#define LORA_SYNC_WORD    0xA2
#define LORA_FREQ         433920000
#define LORA_SF           12
#define LORA_BW           62.5E3
#define LORA_TX_POWER     20

#define LORA_REPLY_REPEAT    5      
#define LORA_REPLY_DELAY_MS  1500UL  

#define LORA_MIN_RSSI       -100


#define LORA_CMD_CONFIRM     2

#define OT_TRIP_COUNT          5
#define WATCHDOG_TIMEOUT_MS    60000UL
#define MOTOR_MAX_RUNTIME_MS  (30UL * 60UL * 1000UL)


#define BOOT_LOCKOUT_MS        3000UL

#define BUTTON_DEBOUNCE_MS     200UL

#define EEPROM_ADDR_DURATION   0

bool          motorOn      = false;
bool          otTripped    = false;
bool          wdgFired     = false;
bool          runtimeFired = false;
int           ot_count     = 0;
unsigned long lastCmdMs    = 0;
unsigned long motorOnMs    = 0;
unsigned long bootMs       = 0;   

int           motorDuration = 30;
unsigned long motorTimerStart = 0;
bool          motorTimerRunning = false;

bool          btnFlag   = false;
unsigned long lastBtnMs = 0;

int  confirmON  = 0;
int  confirmOFF = 0;
int  confirmHB  = 0;

bool          replyPending     = false;
unsigned long replyScheduledAt = 0;



String buildStatusStr() {
  if (otTripped && motorOn) return "11";
  if (otTripped)            return "10";
  if (motorOn)              return "01";
  return "00";
}

void sendStatusReply() {
  String pkt = String(LORA_NETWORK_ID) + LORA_DEVICE_ID + buildStatusStr();
  for (int i = 0; i < LORA_REPLY_REPEAT; i++) {
    LoRa.beginPacket();
    LoRa.print(pkt);
    LoRa.endPacket();
    delay(200);
  }
  Serial.print("[SLAVE] Reply: "); Serial.println(pkt);
}

void scheduleReply() {
  replyPending     = true;
  replyScheduledAt = millis();
}

void checkAndSendReply() {
  if (!replyPending) return;
  if (millis() - replyScheduledAt < LORA_REPLY_DELAY_MS) return;
  replyPending = false;
  sendStatusReply();
}


void doMotorON() {
  if (millis() - bootMs < BOOT_LOCKOUT_MS) {
    Serial.println("[SLAVE] Boot lockout – ON ignored");
    scheduleReply(); return;
  }
  if (otTripped) {
    Serial.println("[SLAVE] OT tripped – ON rejected");
    scheduleReply(); return;
  }
  if (!motorOn) {
    digitalWrite(MOTOR_PIN, HIGH);
    motorOn      = true;
    motorOnMs    = millis();
    motorTimerStart = millis();
    motorTimerRunning = true;
    wdgFired     = false;
    runtimeFired = false;
    Serial.println("[SLAVE] Motor ON");
  }
  scheduleReply();
}

void doMotorOFF(const char *reason) {
  if (motorOn) {
    digitalWrite(MOTOR_PIN, LOW);
    motorOn = false;
    motorTimerRunning = false;
    Serial.print("[SLAVE] Motor OFF – "); Serial.println(reason);
  }
  scheduleReply();
}

void doMotorOFF_immediate(const char *reason) {
  if (motorOn) {
    digitalWrite(MOTOR_PIN, LOW);
    motorOn = false;
    motorTimerRunning = false;
    Serial.print("[SLAVE] Motor OFF – "); Serial.println(reason);
  }
  replyPending = false;
  sendStatusReply();
}



void setupManualButtonMode() {
  if (digitalRead(MANUAL_BUTTON) == LOW) {
    
    motorDuration = EEPROM.read(EEPROM_ADDR_DURATION);
    if (motorDuration < 1 || motorDuration > 180) {
      motorDuration = 30;
    }
    
    int temp_count = 100;
    while (temp_count >= 1) {
      if (digitalRead(MANUAL_BUTTON) == LOW) {
        if (motorDuration <= 180) {
          motorDuration += 5;
          temp_count++;
        } else {
          motorDuration = 0;
        }
      }
      temp_count--;
      delay(200);
      Serial.print("[SLAVE] Programming temp count: ");
      Serial.println(temp_count);
      Serial.print("[SLAVE] Motor Duration: ");
      Serial.println(motorDuration);
      
      EEPROM.write(EEPROM_ADDR_DURATION, motorDuration);
      if (EEPROM.commit()) {
        Serial.println("[SLAVE] EEPROM successfully committed");
      } else {
        Serial.println("[SLAVE] ERROR! EEPROM commit failed");
      }
    }
    
    for (int i = 0; i < 8; i++) {
      delay(200);
      delay(200);
    }
    
    Serial.print("[SLAVE] Motor duration set to: ");
    Serial.println(motorDuration);
  }
  
  if (motorDuration < 1 || motorDuration > 180) {
    motorDuration = 30;
  }
}

void pollManualButton() {
  if (digitalRead(MANUAL_BUTTON) == LOW) {
    unsigned long now = millis();
    if (now - lastBtnMs > BUTTON_DEBOUNCE_MS) {
      lastBtnMs = now;
      btnFlag = true;
    }
  }
}

void handleManualButton() {
  if (millis() - bootMs < BOOT_LOCKOUT_MS) {
    Serial.println("[SLAVE] Button ignored – boot lockout");
    return;
  }
  
  if (otTripped) {
    Serial.println("[SLAVE] Button – resetting OT trip");
    otTripped = false;
    scheduleReply();
    return;
  }
  
  if (motorOn) {
    doMotorOFF("manual button");
  } else {
    doMotorON();
  }
}


void checkMotorTimer() {
  if (!motorTimerRunning) return;
  if (motorOn && (millis() - motorTimerStart) >= (motorDuration * 1000UL)) {
    Serial.println("[SLAVE] Motor timer expired – auto OFF");
    doMotorOFF_immediate("timer expired");
    motorTimerRunning = false;
  }
}


void loraCheckReceive() {
  if (replyPending) return;   

  int pktSize = LoRa.parsePacket();
  if (!pktSize) return;

  String data = LoRa.readString();
  int    rssi = LoRa.packetRssi();

  Serial.print("[SLAVE] RX: '"); Serial.print(data);
  Serial.print("' len="); Serial.print(data.length());
  Serial.print(" RSSI="); Serial.println(rssi);

  if (rssi < LORA_MIN_RSSI) {
    Serial.println("[SLAVE] Packet discarded: RSSI too weak (noise)");
    confirmON = 0; confirmOFF = 0; confirmHB = 0;
    return;
  }

  if (data.length() != 7) {
    Serial.print("[SLAVE] Packet discarded: bad length=");
    Serial.println(data.length());
    confirmON = 0; confirmOFF = 0; confirmHB = 0;
    return;
  }

  String netId = data.substring(0, 4);
  String devId = data.substring(4, 6);
  char   cmd   = data.charAt(6);

  if (!netId.equals(LORA_NETWORK_ID)) {
    Serial.println("[SLAVE] Packet discarded: wrong networkid");
    confirmON = 0; confirmOFF = 0; confirmHB = 0;
    return;
  }
  if (!devId.equals(LORA_DEVICE_ID)) {
    Serial.println("[SLAVE] Packet discarded: wrong deviceid");
    confirmON = 0; confirmOFF = 0; confirmHB = 0;
    return;
  }

  lastCmdMs = millis();

  if (cmd == '1') {
    confirmOFF = 0; confirmHB = 0;
    confirmON++;
    Serial.print("[SLAVE] CMD ON confirm "); Serial.print(confirmON);
    Serial.print("/"); Serial.println(LORA_CMD_CONFIRM);
    if (confirmON >= LORA_CMD_CONFIRM) {
      confirmON = 0;
      doMotorON();
    }
  }
  else if (cmd == '0') {
    confirmON = 0; confirmHB = 0;
    confirmOFF++;
    Serial.print("[SLAVE] CMD OFF confirm "); Serial.print(confirmOFF);
    Serial.print("/"); Serial.println(LORA_CMD_CONFIRM);
    if (confirmOFF >= LORA_CMD_CONFIRM) {
      confirmOFF = 0;
      doMotorOFF("master");
    }
  }
  else if (cmd == '2') {
    confirmON = 0; confirmOFF = 0;
    confirmHB++;
    if (confirmHB >= LORA_CMD_CONFIRM) {
      confirmHB = 0;
      Serial.println("[SLAVE] Heartbeat confirmed");
      scheduleReply();
    }
  }
  else {
    confirmON = 0; confirmOFF = 0; confirmHB = 0;
    Serial.print("[SLAVE] Unknown cmd: "); Serial.println(cmd);
    scheduleReply();
  }
}


void processOTSensor() {
  if (!motorOn) { ot_count = 0; return; }
  if (digitalRead(OT_SENSOR_PIN) == LOW) {
    ot_count++;
    if (ot_count >= OT_TRIP_COUNT) {
      otTripped = true; ot_count = 0;
      doMotorOFF_immediate("OT TRIP");
    }
  } else { ot_count = 0; }
}


void checkHeartbeatWatchdog() {
  if (!motorOn) return;
  if (millis() - lastCmdMs < WATCHDOG_TIMEOUT_MS) return;
  Serial.println("[SLAVE/WDG] Timeout – Motor OFF");
  wdgFired = true;
  doMotorOFF_immediate("heartbeat watchdog");
  lastCmdMs = millis();
}


void checkRuntimeWatchdog() {
  if (!motorOn) return;
  if (millis() - motorOnMs < MOTOR_MAX_RUNTIME_MS) return;
  Serial.println("[SLAVE/RT] Runtime limit – Motor OFF");
  runtimeFired = true;
  doMotorOFF_immediate("runtime limit");
  motorOnMs = millis();
}


void printStatus() {
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint < 10000UL) return;
  lastPrint = millis();
  unsigned long sinceCmd = (millis() - lastCmdMs) / 1000UL;
  unsigned long runSec   = motorOn ? (millis() - motorOnMs) / 1000UL : 0;
  bool inLockout = (millis() - bootMs < BOOT_LOCKOUT_MS);
  Serial.println("────────────────────────────────");
  Serial.print("Motor  : "); Serial.println(motorOn      ? "ON"   : "OFF");
  Serial.print("OT     : "); Serial.println(otTripped    ? "TRIP" : "ok");
  Serial.print("WDG    : "); Serial.println(wdgFired     ? "YES"  : "no");
  Serial.print("RT     : "); Serial.println(runtimeFired ? "YES"  : "no");
  Serial.print("Lockout: "); Serial.println(inLockout    ? "YES"  : "no");
  Serial.print("Motor Dur:"); Serial.print(motorDuration); Serial.println(" sec");
  Serial.print("SinceCmd: "); Serial.print(sinceCmd);
  Serial.print("s / WDG="); Serial.print(WATCHDOG_TIMEOUT_MS/1000UL); Serial.println("s");
  if (motorOn) {
    Serial.print("Runtime: "); Serial.print(runSec);
    Serial.print("s / cap="); Serial.print(MOTOR_MAX_RUNTIME_MS/1000UL); Serial.println("s");
    unsigned long timerRemaining = (motorDuration * 1000UL - (millis() - motorTimerStart)) / 1000UL;
    if (motorTimerRunning && timerRemaining > 0) {
      Serial.print("Timer remaining: "); Serial.print(timerRemaining); Serial.println("s");
    }
  }
  Serial.println("────────────────────────────────");
}


void setup() {
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);   

  bootMs = millis();   

  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== LORA SLAVE BOARD ===");

  EEPROM.begin(512);
  
  pinMode(MANUAL_BUTTON, INPUT_PULLUP);
  
  motorDuration = EEPROM.read(EEPROM_ADDR_DURATION);
  if (motorDuration < 1 || motorDuration > 180) {
    motorDuration = 30;
    EEPROM.write(EEPROM_ADDR_DURATION, motorDuration);
    EEPROM.commit();
  }
  
  setupManualButtonMode();

  pinMode(OT_SENSOR_PIN, INPUT_PULLUP);

  lastCmdMs = millis();

  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);

  Serial.print("LoRa init");
  int attempt = 0;
  while (!LoRa.begin(LORA_FREQ)) {
    Serial.print(".");
    if (++attempt >= 30) break;
    delay(500);
  }
  if (attempt < 30) Serial.println(" OK");
  else {
    Serial.println(" FAILED");
    Serial.println("Check: NSS=D8 RST=D0 DIO0=D4 MOSI=D7 MISO=D6 SCK=D5");
  }

  Serial.print("Min RSSI:    "); Serial.println(LORA_MIN_RSSI);
  Serial.print("Confirm:     "); Serial.print(LORA_CMD_CONFIRM); Serial.println("x");
  Serial.print("Reply delay: "); Serial.print(LORA_REPLY_DELAY_MS); Serial.println("ms");
  Serial.print("Boot lockout:"); Serial.print(BOOT_LOCKOUT_MS);  Serial.println("ms");
  Serial.print("Motor duration:"); Serial.print(motorDuration); Serial.println(" seconds");
  Serial.println("Ready – waiting for packets");
  Serial.println("Manual button on D9: short press toggles motor, press during boot to set duration");
}


void loop() {
  loraCheckReceive();
  checkAndSendReply();
  processOTSensor();
  checkHeartbeatWatchdog();
  checkRuntimeWatchdog();
  checkMotorTimer();       
  pollManualButton();      
  if (btnFlag) { 
    btnFlag = false; 
    handleManualButton();   
  }
  printStatus();
  delay(10);
}