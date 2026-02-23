#include <Arduino.h>
#include <EEPROM.h>
#include <LoRa.h>
#include <SPI.h>
#include <espnow.h>
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif

#define EEPROM_SIZE 256
#define EEPROM_START_UNIX_ADDR   0   
#define EEPROM_STOP_UNIX_ADDR    4
#define EEPROM_START_UNIX1_ADDR  8   
#define EEPROM_STOP_UNIX1_ADDR   12

#define MOTOR1_PIN D2       
#define LORA_STATUS_LED LED_BUILTIN     
#define OT_SENSOR_PIN D1
#define MOTOR1_BUTTON D9

#define OT_ACTIVE_LEVEL LOW
int ot_sensorcount = 0;
const int OT_TRIP_COUNT = 3;

bool motor1_status_manual = 0;
bool motor2_status_manual = 0;

unsigned long mili_now;
unsigned long lastExecutionTime = 0;
unsigned long lastLoraExecutionTime = 0;
const unsigned long EXECUTION_INTERVAL = 10000;     
const unsigned long LORA_INTERVAL = 15000;         
const unsigned long POST_SCHEDULE_LORA_TIME = 120000; 
uint32_t baseUnixTime = 0;
unsigned long baseMillis = 0;




uint32_t lastSavedStartUnix = 0;
uint32_t lastSavedStopUnix = 0;
uint32_t lastSavedStartUnix1 = 0;
uint32_t lastSavedStopUnix1 = 0;

struct Schedule {
  time_t unixTime;
  bool triggered;
};

uint8_t scheduleCount = 3;

struct sch {
  uint32_t startUnix;
  uint32_t stopUnix;
  uint8_t startTime;
  uint8_t stopTime;
  bool active;
  int state;
  int ack;
  int sch1_en;
};

struct sche {
  uint32_t startUnix1;
  uint32_t stopUnix1;
  uint8_t startTime1;
  uint8_t stopTime1;
  bool active1;
  int state1;
  int ack1;
  int sch1_en1;
  bool sendPostScheduleFull;
  unsigned long postScheduleEndTime;
};

sch sch1;
sch updated_sch1;
sche sche1;
sche updated_sche1;

bool scheduleCancelledByApp = false;
bool scheduleCancelledByApp1 = false;
volatile bool espNowDataReceived = false;


#define LORA_SS 15
#define LORA_RST 16
#define LORA_DIO0 2
#define LORA_FREQUENCY 433920000

String loraDeviceId = "210001";
int loraVstate1 = 1;
int loraVstate2 = 1;
bool loraInitialized = false;

int lastAppState = -1;
int lastAppState1 = -1;

int lastDay = -1;
int lastDay1 = -1;

uint8_t wifiMacAddress[] = {0x8C, 0x4F, 0x00, 0xE1, 0xE0, 0x75}; // Device 1 MAC
uint8_t partnerMacAddress[] = {0x84, 0x0D, 0x8E, 0xB8, 0x47, 0xB9}; // Device 2 MAC

typedef struct esp_now_command {
  uint32_t currentUnixTime;
  int motor1AppState;
  uint32_t motor1StartUnix;
  uint32_t motor1StopUnix;
  int motor1Ack;
  int motor1SchEn;
  int motor2AppState;
  uint32_t motor2StartUnix;
  uint32_t motor2StopUnix;
  int motor2Ack;
  int motor2SchEn;
} esp_now_command;

typedef struct esp_now_feedback {
  int motor1PhysicalState;
  int motor1DeviceState;
  uint8_t motor1Cancellation;
  int motor2State;
  int motor2DeviceState;
  uint8_t motor2Cancellation;
  int heartBeatCount;
} esp_now_feedback;

esp_now_command receivedCommand;
esp_now_feedback feedbackData;

void initializeLORA();
void sendLORAData();
void loadSchedules();
void saveScheduleToEEPROM();
void loadScheduleFromEEPROM();
void loadSchedules1();
void saveScheduleToEEPROM1();
void loadScheduleFromEEPROM1();
void checkSch(uint32_t nowUnix);
void checkSch1(uint32_t nowUnix);
void process_LocalEvents();
void process_LoraEvents();
void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len);
void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus);
void sendFeedbackToDevice2();


void initESP_NOW() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW initialization failed");
    return;
  }
  
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_register_send_cb(OnDataSent);
  
  esp_now_add_peer(partnerMacAddress, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  
  Serial.println("ESP-NOW Initialized");
}

void OnDataRecv(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  memcpy(&receivedCommand, incomingData, sizeof(receivedCommand));
    espNowDataReceived = true;
}
uint32_t getCurrentUnix() {
  return baseUnixTime + ((millis() - baseMillis) / 1000);
}
  bool manualMotor1On = false;
void processEspNowCommand() {
baseUnixTime = receivedCommand.currentUnixTime;
baseMillis = millis();
  Serial.println("Received data from Device 2:");
  Serial.print("Current Time: "); Serial.println(receivedCommand.currentUnixTime);
  Serial.print("Motor1 App State: "); Serial.println(receivedCommand.motor1AppState);
  Serial.print("Motor1 Start: "); Serial.println(receivedCommand.motor1StartUnix);
  Serial.print("Motor1 Stop: "); Serial.println(receivedCommand.motor1StopUnix);
  
  if (!sch1.active) {
    if (receivedCommand.motor1AppState == 1 && digitalRead(MOTOR1_PIN) == LOW) {
      digitalWrite(MOTOR1_PIN, HIGH);
      Serial.println("Motor1 ON from App via ESP-NOW");
      feedbackData.motor1PhysicalState = 1;
      feedbackData.motor1DeviceState = 1;
      manualMotor1On = true;
      scheduleCancelledByApp = false;

    }
    else if (receivedCommand.motor1AppState == 0 && digitalRead(MOTOR1_PIN) == HIGH) {
      digitalWrite(MOTOR1_PIN, LOW);
      Serial.println("Motor1 OFF from App via ESP-NOW");
      manualMotor1On = false;
      scheduleCancelledByApp = true;

      feedbackData.motor1PhysicalState = 0;
      feedbackData.motor1DeviceState = 0;
    }
  }
  
  updated_sch1.state = receivedCommand.motor1AppState;
  sch1.ack = receivedCommand.motor1Ack;
  sch1.sch1_en = receivedCommand.motor1SchEn;
  
  if (sch1.startUnix != receivedCommand.motor1StartUnix || 
      sch1.stopUnix != receivedCommand.motor1StopUnix) {
    sch1.startUnix = receivedCommand.motor1StartUnix;
    sch1.stopUnix = receivedCommand.motor1StopUnix;
    saveScheduleToEEPROM();
  }
  
  bool appOffPressed = (lastAppState == 1 && receivedCommand.motor1AppState == 0);
  lastAppState = receivedCommand.motor1AppState;
  
  if (sch1.active && appOffPressed) {
    sch1.active = false;
    scheduleCancelledByApp = true;
    digitalWrite(MOTOR1_PIN, LOW);
    feedbackData.motor1Cancellation = true;
    Serial.println("Motor1 Schedule cancelled by App via ESP-NOW");
  }
  
  if (scheduleCancelledByApp && receivedCommand.motor1StartUnix > receivedCommand.currentUnixTime) {
    scheduleCancelledByApp = false;
    Serial.println("Motor1: New schedule detected - cancellation cleared");
  }
  
  updated_sche1.state1 = receivedCommand.motor2AppState;
  sche1.ack1 = receivedCommand.motor2Ack;
  sche1.sch1_en1 = receivedCommand.motor2SchEn;
  
  if (sche1.startUnix1 != receivedCommand.motor2StartUnix || 
      sche1.stopUnix1 != receivedCommand.motor2StopUnix) {
    sche1.startUnix1 = receivedCommand.motor2StartUnix;
    sche1.stopUnix1 = receivedCommand.motor2StopUnix;
    saveScheduleToEEPROM1();
  }
  
  if (sche1.state1 != receivedCommand.motor2AppState) {
    sche1.state1 = receivedCommand.motor2AppState;
    
    if (sche1.state1 == 1) {
      loraVstate1 = 0;
      loraVstate2 = 0;
      feedbackData.motor2State = 1;
      feedbackData.motor2DeviceState = 1;
      Serial.println("Motor2 App ON via ESP-NOW");
    } 
    else if (sche1.state1 == 0) {
      loraVstate1 = 1;
      loraVstate2 = 1;
      feedbackData.motor2State = 0;
      feedbackData.motor2DeviceState = 0;
      Serial.println("Motor2 App OFF via ESP-NOW");
    }
    
    sendLORAData();
  }
  
  bool appOffPressed1 = (lastAppState1 == 1 && receivedCommand.motor2AppState == 0);
  lastAppState1 = receivedCommand.motor2AppState;
  
  if (sche1.active1 && appOffPressed1) {
    sche1.active1 = false;
    scheduleCancelledByApp1 = true;
    sche1.sendPostScheduleFull = true;
    sche1.postScheduleEndTime = millis() + POST_SCHEDULE_LORA_TIME;
    loraVstate1 = 1;
    loraVstate2 = 1;
    feedbackData.motor2Cancellation = true;
    Serial.println("Motor2 Schedule cancelled by App via ESP-NOW");
  }
  
  if (scheduleCancelledByApp1 && receivedCommand.motor2StartUnix > receivedCommand.currentUnixTime) {
    scheduleCancelledByApp1 = false;
    Serial.println("Motor2: New schedule detected - cancellation cleared");
  }
  
  sendFeedbackToDevice2();
}

void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  if (sendStatus == 0) {
    Serial.println("ESP-NOW Send success");
  } else {
    Serial.println("ESP-NOW Send failed");
  }
}

void sendFeedbackToDevice2() {
  feedbackData.motor1PhysicalState = digitalRead(MOTOR1_PIN) == HIGH ? 1 : 0;
  feedbackData.heartBeatCount = 10; 
  
  esp_now_send(partnerMacAddress, (uint8_t *)&feedbackData, sizeof(feedbackData));
  Serial.println("Feedback sent to Device 2");
}


void loadSchedules() {
  sch1 = {
    1767225600,  
    1767225600,  
    0,
    0,
    false,
    0,
    0,
    0
  };
}

void saveScheduleToEEPROM() {
  if (sch1.startUnix != lastSavedStartUnix ||
      sch1.stopUnix != lastSavedStopUnix) {
    EEPROM.put(EEPROM_START_UNIX_ADDR, sch1.startUnix);
    EEPROM.put(EEPROM_STOP_UNIX_ADDR, sch1.stopUnix);
    EEPROM.commit();
    lastSavedStartUnix = sch1.startUnix;
    lastSavedStopUnix = sch1.stopUnix;
    Serial.println("Motor1 schedule saved to EEPROM");
  }
}

void loadScheduleFromEEPROM() {
  EEPROM.get(EEPROM_START_UNIX_ADDR, sch1.startUnix);
  EEPROM.get(EEPROM_STOP_UNIX_ADDR, sch1.stopUnix);

  if (sch1.startUnix < 1000000000 || sch1.stopUnix < sch1.startUnix) {
    Serial.println("Invalid EEPROM data for Motor1, using defaults");
    loadSchedules();
    scheduleCancelledByApp = false;
  }

  sch1.active = false;
  lastSavedStartUnix = sch1.startUnix;
  lastSavedStopUnix = sch1.stopUnix;
}



void checkSch(uint32_t nowUnix) {


  if (manualMotor1On == true) {

    if (!sch1.active) {
      Serial.println("MOTOR1 MANUAL ON");
    }

    sch1.active = false;  
    digitalWrite(MOTOR1_PIN, HIGH);

    feedbackData.motor1PhysicalState = 1;
    feedbackData.motor1DeviceState = 1;

    return;  
  }



  if (scheduleCancelledByApp) {

    if (sch1.active) {
      Serial.println("MOTOR1 CANCELLED BY APP");
    }

    sch1.active = false;
    digitalWrite(MOTOR1_PIN, LOW);

    feedbackData.motor1PhysicalState = 0;
    feedbackData.motor1DeviceState = 0;

    return;
  }


  if (!sch1.active &&
      nowUnix >= sch1.startUnix &&
      nowUnix < sch1.stopUnix &&
      sch1.ack == 1 &&
      sch1.sch1_en == 1) {

    sch1.active = true;

    digitalWrite(MOTOR1_PIN, HIGH);

    Serial.println("MOTOR1 SCHEDULE START");

    feedbackData.motor1PhysicalState = 1;
    feedbackData.motor1DeviceState = 1;
  }



  if (sch1.active && nowUnix >= sch1.stopUnix) {

    sch1.active = false;

    digitalWrite(MOTOR1_PIN, LOW);

    Serial.println("MOTOR1 SCHEDULE STOP");

    feedbackData.motor1PhysicalState = 0;
    feedbackData.motor1DeviceState = 0;
  }
}



void loadSchedules1() {
  sche1 = {
    1767225600,  
    1767225600,  
    0,           
    0,           
    false,       
    0,           
    0,           
    0,          
    false,       
    0            
  };
}

void saveScheduleToEEPROM1() {
  if (sche1.startUnix1 != lastSavedStartUnix1 ||
      sche1.stopUnix1 != lastSavedStopUnix1) {
    EEPROM.put(EEPROM_START_UNIX1_ADDR, sche1.startUnix1);
    EEPROM.put(EEPROM_STOP_UNIX1_ADDR, sche1.stopUnix1);
    EEPROM.commit();
    lastSavedStartUnix1 = sche1.startUnix1;
    lastSavedStopUnix1 = sche1.stopUnix1;
    Serial.println("Motor2 schedule saved to EEPROM");
  }
}

void loadScheduleFromEEPROM1() {
  EEPROM.get(EEPROM_START_UNIX1_ADDR, sche1.startUnix1);
  EEPROM.get(EEPROM_STOP_UNIX1_ADDR, sche1.stopUnix1);

  if (sche1.startUnix1 < 1000000000 || sche1.stopUnix1 < sche1.startUnix1) {
    Serial.println("Invalid EEPROM data for Motor2, using defaults");
    loadSchedules1();
    scheduleCancelledByApp1 = false;
  }

  sche1.active1 = false;
  sche1.sendPostScheduleFull = false;
  lastSavedStartUnix1 = sche1.startUnix1;
  lastSavedStopUnix1 = sche1.stopUnix1;
}

void checkSch1(uint32_t nowUnix) {
  if (!sche1.active1 &&
      !scheduleCancelledByApp1 &&
      nowUnix >= sche1.startUnix1 &&
      nowUnix < sche1.stopUnix1 &&
      sche1.ack1 == 1 &&
      sche1.sch1_en1 == 1) {
    sche1.active1 = true;
    sche1.sendPostScheduleFull = false;
    loraVstate1 = 0;
    loraVstate2 = 0;
    feedbackData.motor2State = 1;
    feedbackData.motor2DeviceState = 1;
    Serial.println("MOTOR2 SCHEDULE START");
  }

  else if (sche1.active1 && nowUnix >= sche1.stopUnix1) {
    sche1.active1 = false;
    sche1.state1 = 0;
    sche1.sendPostScheduleFull = true;
    sche1.postScheduleEndTime = millis() + POST_SCHEDULE_LORA_TIME;
    loraVstate1 = 1;
    loraVstate2 = 1;
    feedbackData.motor2State = 0;
    feedbackData.motor2DeviceState = 0;
    feedbackData.motor2Cancellation = true;
    sendFeedbackToDevice2();

    Serial.println("MOTOR2 SCHEDULE STOP");
  }
}


void initializeLORA() {
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  LoRa.setSyncWord(0xA2);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(62.5E3);
  
  if (!LoRa.begin(LORA_FREQUENCY)) {
    Serial.println("LORA initialization failed!");
    loraInitialized = false;
    return;
  }
  
  loraInitialized = true;
  Serial.println("LORA Initialized OK!");
}

void sendLORAData() {
  if (!loraInitialized) {
    Serial.println("LORA not initialized!");
    return;
  }

  if (sche1.active1) {
    loraVstate1 = 0;
    loraVstate2 = 0;
    Serial.println("LORA: Sending EMPTY signal (schedule active)");
  } 
  else if (sche1.sendPostScheduleFull && millis() < sche1.postScheduleEndTime) {
    loraVstate1 = 1;
    loraVstate2 = 1;
    Serial.println("LORA: Sending FULL signal (post-schedule)");
  } 
  else if (sche1.sendPostScheduleFull && millis() >= sche1.postScheduleEndTime) {
    sche1.sendPostScheduleFull = false;
    
    if (sche1.state1 == 1) {
      loraVstate1 = 0;
      loraVstate2 = 0;
      Serial.println("LORA: Sending EMPTY signal (app ON)");
    } else {
      loraVstate1 = 1;
      loraVstate2 = 1;
      Serial.println("LORA: Sending FULL signal (app OFF)");
    }
  } 
  else {
    if (sche1.state1 == 1) {
      loraVstate1 = 0;
      loraVstate2 = 0;
      Serial.println("LORA: Sending EMPTY signal (app ON)");
    } else {
      loraVstate1 = 1;
      loraVstate2 = 1;
      Serial.println("LORA: Sending FULL signal (app OFF)");
    }
  }

  for(int i=0;i<=(5);i++){
  LoRa.beginPacket();
  LoRa.print(loraDeviceId);
  LoRa.print(loraVstate1);
  LoRa.print(loraVstate2);
  LoRa.endPacket();

  Serial.print(".");
  Serial.print(loraDeviceId);
  Serial.print(loraVstate1);
  Serial.print(loraVstate2);
  delay(50);
  }

}


void process_LocalEvents() {    
  static int lastDay = -1;
  uint32_t nowUnix = getCurrentUnix(); 

  if (lastDay == -1) lastDay = nowUnix / 86400; 

  if ((nowUnix / 86400) != lastDay) {
    scheduleCancelledByApp = false;
    lastDay = nowUnix / 86400;
    Serial.println("Motor1: New day – app cancellation reset");
  }

  if (digitalRead(MOTOR1_BUTTON) == LOW) {
    delay(50); 

    if (digitalRead(MOTOR1_BUTTON) == LOW) {
      Serial.println("Motor1 Button Pressed");

      if (sch1.active && digitalRead(MOTOR1_PIN) == HIGH) {
        sch1.active = false;
        scheduleCancelledByApp = true;
        feedbackData.motor1Cancellation = true;
        Serial.println("Motor1 Schedule cancelled by Manual Button");
      }

      if (digitalRead(MOTOR1_PIN) == HIGH) {
        digitalWrite(MOTOR1_PIN, LOW);
        motor1_status_manual = 0;
        feedbackData.motor1PhysicalState = 0;
        feedbackData.motor1DeviceState = 0;
        Serial.println("Motor1 OFF by Button");
      } else {
        digitalWrite(MOTOR1_PIN, HIGH);
        motor1_status_manual = 1;
        feedbackData.motor1PhysicalState = 1;
        feedbackData.motor1DeviceState = 1;
        Serial.println("Motor1 ON by Button");
      }
      
      sendFeedbackToDevice2();
      delay(500); 
    }
  }

  if (mili_now - lastExecutionTime >= EXECUTION_INTERVAL) {
    Serial.println("\n=== Processing Motor1 Events ===");
    lastExecutionTime = mili_now;
    
    checkSch(nowUnix);
    
    Serial.print("Current Unix Time (from Device 2): ");
    Serial.println(nowUnix);
    
    feedbackData.heartBeatCount = 10;
    sendFeedbackToDevice2();
  }
}

void process_LoraEvents() {
  static int lastDay1 = -1;
  uint32_t nowUnix = getCurrentUnix(); 

  if (lastDay1 == -1) lastDay1 = nowUnix / 86400; 

  if ((nowUnix / 86400) != lastDay1) {
    scheduleCancelledByApp1 = false;
    lastDay1 = nowUnix / 86400;
    Serial.println("Motor2: New day - app cancellation reset");
  }

  checkSch1(nowUnix);

  if (mili_now - lastLoraExecutionTime >= LORA_INTERVAL) {
    Serial.println("\n=== Processing Motor2 LORA Events ===");
    lastLoraExecutionTime = mili_now;
    
    Serial.print("Current Time (from Device 2): ");
    Serial.println(nowUnix);
    
    sendLORAData();
  }
}


void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  pinMode(MOTOR1_PIN, OUTPUT);
  digitalWrite(MOTOR1_PIN, LOW);
  pinMode(MOTOR1_BUTTON, INPUT_PULLUP);
  pinMode(LORA_STATUS_LED, OUTPUT);
  pinMode(OT_SENSOR_PIN, INPUT_PULLUP);
  
  loadScheduleFromEEPROM();   
  loadScheduleFromEEPROM1();  
  
  initializeLORA();
  
  initESP_NOW();
  
  feedbackData.motor1PhysicalState = 0;
  feedbackData.motor1DeviceState = 0;
  feedbackData.motor1Cancellation = false;
  feedbackData.motor2State = 0;
  feedbackData.motor2DeviceState = 0;
  feedbackData.motor2Cancellation = false;
  feedbackData.heartBeatCount = 0;
  
  Serial.println("Device 1 (Motor+LORA) setup complete");
}

void loop() {
  mili_now = millis();
  if (espNowDataReceived) {
    espNowDataReceived = false;
    processEspNowCommand();
}
  
  process_LocalEvents();
  
  process_LoraEvents();
  
  delay(100);
}