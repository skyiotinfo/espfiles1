

#include <Arduino.h>
#include <LoRa.h>
#include <SPI.h>


#define MOTOR1_PIN      D2 
#define OT_SENSOR_PIN   D1   
#define BUTTON_PIN      D3   

#define LORA_NSS    15   // GPIO15 = D8
#define LORA_RST    16   // GPIO16 = D0
#define LORA_DIO0    2   // GPIO2  = D4

#define BOOT_LOCKOUT_MS  3000UL

#define LORA_NETWORK_ID  "2015"
#define LORA_DEVICE_ID   "10"
#define LORA_SYNC_WORD   0xA2
#define LORA_FREQ        433920000   
#define LORA_SF          12
#define LORA_BW          62.5E3
#define LORA_TX_POWER    20

#define LORA_SEND_REPEAT     5
#define LORA_ACK_TIMEOUT_MS  8000UL   // wait up to 8s for slave reply
#define LORA_MAX_RETRIES     3
#define LORA_HB_INTERVAL_MS  20000UL  // heartbeat every 20s

bool          loraWaiting  = false;
unsigned long loraSentAt   = 0;
char          loraLastCmd  = '0';
uint8_t       loraRetry    = 0;
unsigned long loraLastHbMs = 0;

bool motor1On = false;
bool motor1OT = false;
int  ot_count = 0;
#define OT_TRIP_COUNT  5

bool motor2On = false;
bool motor2OT = false;

unsigned long bootMs = 0;  

bool          btnFlag   = false;
unsigned long lastBtnMs = 0;

unsigned long lastStatusMs = 0;
#define STATUS_INTERVAL_MS  3000UL

void loraSendCmd(char cmd);
void sendStatus();

//  HARDWARE SERIAL ↔ INTERNET BOARD
//  Format sent:     "S:M1:X:M2:X:OT1:X:OT2:X\n"
//  Format received: "M1:1\n" "M1:0\n" "M2:1\n" "M2:0\n" "HB\n"

void sendStatus() {
  char buf[40];
  snprintf(buf, sizeof(buf), "S:M1:%d:M2:%d:OT1:%d:OT2:%d",
           (int)motor1On, (int)motor2On, (int)motor1OT, (int)motor2OT);
  Serial.println(buf);
}

void parseLineFromInternet(String &line) {
  if (line.length() < 2) return;
  if (line == "HB")   { sendStatus(); return; }
  if (line == "M1:1") {
    if (millis() - bootMs > BOOT_LOCKOUT_MS && !motor1OT) {
      digitalWrite(MOTOR1_PIN, HIGH);
      motor1On = true;
    }
    sendStatus(); return;
  }
  if (line == "M1:0") { digitalWrite(MOTOR1_PIN, LOW); motor1On = false; sendStatus(); return; }
  if (line == "M2:1") { loraSendCmd('1'); return; }
  if (line == "M2:0") { loraSendCmd('0'); return; }
}

void pollInternetSerial() {
  static String rxBuf = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      rxBuf.trim();
      if (rxBuf.length() > 0) parseLineFromInternet(rxBuf);
      rxBuf = "";
    } else if (c != '\r') {
      rxBuf += c;
      if (rxBuf.length() > 60) rxBuf = "";
    }
  }
}


void loraSendCmd(char cmd) {
  for (int i = 0; i < LORA_SEND_REPEAT; i++) {
    LoRa.beginPacket();
    LoRa.print(LORA_NETWORK_ID);
    LoRa.print(LORA_DEVICE_ID);
    LoRa.print(cmd);
    LoRa.endPacket();
    delay(200);
  }
  loraLastCmd = cmd;
  loraWaiting = true;
  loraSentAt  = millis();
  loraRetry   = 0;
  loraLastHbMs = millis();
}

void loraSendHeartbeat() {
  if (loraWaiting) return;
  unsigned long now = millis();
  if (now - loraLastHbMs < LORA_HB_INTERVAL_MS) return;
  loraLastHbMs = now;
  for (int i = 0; i < LORA_SEND_REPEAT; i++) {
    LoRa.beginPacket();
    LoRa.print(LORA_NETWORK_ID);
    LoRa.print(LORA_DEVICE_ID);
    LoRa.print('2');
    LoRa.endPacket();
    delay(200);
  }
}

//  LoRa RECEIVE – parse slave reply
//  Slave sends: networkid(4) + deviceid(2) + status(2) = 8 chars
//  status: "01"=ON  "00"=OFF  "10"=OT  "11"=OT+ON

void loraCheckReceive() {
  int pktSize = LoRa.parsePacket();
  if (!pktSize) return;

  String data = LoRa.readString();

  if (data.length() < 8) return;
  String netId  = data.substring(0, 4);
  String devId  = data.substring(4, 6);
  String status = data.substring(6, 8);

  if (netId != LORA_NETWORK_ID) return;
  if (devId != LORA_DEVICE_ID)  return;

  loraWaiting = false;

  bool slaveOn = (status == "01" || status == "11");
  bool slaveOT = (status == "10" || status == "11");

  if (slaveOT && !motor2OT) {
    motor2OT = true;
    motor2On = false;
    sendStatus();
    return;
  }

  motor2On = slaveOn;
  motor2OT = slaveOT;
  sendStatus();
}

void loraHandleRetry() {
  if (!loraWaiting) return;
  if (millis() - loraSentAt < LORA_ACK_TIMEOUT_MS) return;

  if (loraRetry >= LORA_MAX_RETRIES) {
    loraWaiting = false;
    if (loraLastCmd == '1') motor2On = false;
    sendStatus();
    return;
  }

  loraRetry++;
  loraSentAt = millis();
  for (int i = 0; i < LORA_SEND_REPEAT; i++) {
    LoRa.beginPacket();
    LoRa.print(LORA_NETWORK_ID);
    LoRa.print(LORA_DEVICE_ID);
    LoRa.print(loraLastCmd);
    LoRa.endPacket();
    delay(200);
  }
}


void processOTSensor() {
  if (!motor1On) { ot_count = 0; return; }
  if (digitalRead(OT_SENSOR_PIN) == LOW) {
    ot_count++;
    if (ot_count >= OT_TRIP_COUNT) {
      digitalWrite(MOTOR1_PIN, LOW);
      motor1On = false; motor1OT = true; ot_count = 0;
      sendStatus();
    }
  } else { ot_count = 0; }
}


void pollButton() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    unsigned long now = millis();
    if (now - lastBtnMs > 200UL) { lastBtnMs = now; btnFlag = true; }
  }
}

void handleButton() {
  if (millis() - bootMs < BOOT_LOCKOUT_MS) return;  
  if (motor1OT) { motor1OT = false; sendStatus(); return; }
  if (motor1On) { digitalWrite(MOTOR1_PIN, LOW);  motor1On = false; }
  else          { digitalWrite(MOTOR1_PIN, HIGH); motor1On = true;  }
  sendStatus();
}


void setup() {

  digitalWrite(MOTOR1_PIN, LOW);
  pinMode(MOTOR1_PIN, OUTPUT);
  digitalWrite(MOTOR1_PIN, LOW);   
  bootMs = millis();


  Serial.begin(9600);
  delay(300);

  pinMode(OT_SENSOR_PIN, INPUT_PULLUP);
  pinMode(BUTTON_PIN,    INPUT_PULLUP);

  LoRa.setPins(LORA_NSS, LORA_RST, LORA_DIO0);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setTxPower(LORA_TX_POWER);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setSignalBandwidth(LORA_BW);

  int attempt = 0;
  while (!LoRa.begin(LORA_FREQ)) {
    attempt++;
    if (attempt >= 30) break;
    delay(500);
  }

  loraLastHbMs = millis();
}


void loop() {
  pollInternetSerial();
  pollButton();
  if (btnFlag) { btnFlag = false; handleButton(); }
  processOTSensor();
  loraCheckReceive();
  loraHandleRetry();
  loraSendHeartbeat();

  unsigned long now = millis();
  if (now - lastStatusMs >= STATUS_INTERVAL_MS) {
    lastStatusMs = now;
    sendStatus();
  }

  delay(10);
}
