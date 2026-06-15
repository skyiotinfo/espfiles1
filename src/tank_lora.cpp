#include <EEPROM.h>
#include <Arduino.h>
#include <TM1637Display.h>
#include <LoRa.h>
#include <SPI.h>

// ===================== Pin Definitions =====================
// Display
#define CLK    D3
#define DIO    D4

// LoRa
#define SS     D8
#define RST    D0
#define DIO0   D4          // Note: shares D4 with display DIO – works if display is only written occasionally

// Sensors & Actuators
#define LOW_WATER_PIN   D1   // GPIO5 – HIGH = water present, LOW = water absent
#define BUZZER_PIN      D2   // Also used as motor relay output (active HIGH)
#define MANUAL_BUTTON   D9   // Input pull‑up, LOW when pressed

// ===================== Global Variables =====================
int addr1 = 0;
int value1 = 1;
byte memval1;
int sensor_status = 2;      // 0 = tank full, 1 = tank empty, 2 = no data

int lora_pac_count = 0;
int tcount1 = 0;            // debounce for motor ON command
int tcount2 = 0;            // debounce for motor OFF command
int tcount3 = 0;            // debounce for "no data"

int motor_status = 0;       // 0 = motor off, 1 = motor on
int motor_time = 0;         // remaining seconds
int empty_start = 0;
int value_count = 0;
int motor_duration = 30;    // minutes (set via EEPROM)
int motor_stoptime = 0;
int count = 0;
int sound = 0;
int temp_count1 = 0;        // used for auto‑stop when tank full

// Display patterns
const uint8_t seg_empty[] = {
  0x00,
  SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,
  SEG_A | SEG_D | SEG_E | SEG_F | SEG_G,
  0x00
};

const uint8_t seg_full[] = {
  0x00,
  SEG_A | SEG_E | SEG_F | SEG_G,
  SEG_A | SEG_E | SEG_F | SEG_G,
  0x00
};

const uint8_t seg_nodata[] = {
  SEG_A | SEG_E | SEG_F | SEG_G,
  SEG_A | SEG_B | SEG_E | SEG_F | SEG_G,
  SEG_D | SEG_E | SEG_F,
  SEG_A | SEG_D | SEG_E | SEG_F | SEG_G
};

TM1637Display display(CLK, DIO);
uint8_t blank[] = { 0x00, 0x00, 0x00, 0x00 };

// ===================== Helper Functions =====================
bool isWaterPresent() {
  return digitalRead(LOW_WATER_PIN) == HIGH;
}

void setMotor(bool on) {
  if (on) {
    if (!isWaterPresent()) {
      Serial.println("Motor start blocked: No water (low sensor active)");
      display.showNumberDec(0, false, 1, 0);   // show "0" to indicate block
      display.showNumberDec(0, false);
      delay(1000);
      display.clear();
      return;
    }
    digitalWrite(BUZZER_PIN, HIGH);
    motor_status = 1;
    motor_time = motor_duration * 60;   // convert minutes to seconds
    Serial.println("Motor ON");
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    motor_status = 0;
    display.showNumberDec(0, false);
    Serial.println("Motor OFF");
  }
}

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);

  pinMode(MANUAL_BUTTON, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LOW_WATER_PIN, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);
  motor_status = 0;
  delay(1000);

  // Read motor duration from EEPROM
  memval1 = EEPROM.read(addr1);
  motor_duration = memval1;

  display.setBrightness(0x0f);
  display.setSegments(blank);

  // --- Manual duration setting at startup ---
  int temp_count = 100;
  if (digitalRead(MANUAL_BUTTON) == LOW) {
    while (temp_count >= 1) {
      if (digitalRead(MANUAL_BUTTON) == LOW) {
        if (motor_duration <= 180) {
          motor_duration += 5;
          temp_count++;
        } else {
          motor_duration = 0;
        }
      }
      temp_count--;
      delay(200);
      Serial.print("Temp Count:"); Serial.println(temp_count);
      Serial.print("Motor Duration:"); Serial.println(motor_duration);

      EEPROM.write(addr1, motor_duration);
      if (EEPROM.commit()) {
        Serial.println("EEPROM successfully committed");
        display.showNumberDec(5, false, 1, 0);
        display.showNumberDec(motor_duration, false);
      } else {
        Serial.println("ERROR! EEPROM commit failed");
      }
    }
  }

  if (motor_duration < 1 || motor_duration > 180) {
    motor_duration = 30;
  }

  motor_time = motor_duration * 60;
  empty_start = 0;

  // Flash the duration on display
  for (int i = 0; i < 8; i++) {
    display.showNumberDec(motor_duration, false);
    delay(200);
    display.clear();
    delay(200);
  }
  display.showNumberDec(0, false);

  // --- LoRa Initialisation ---
  LoRa.setPins(SS, RST, DIO0);
  LoRa.setSyncWord(0xA2);
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(62.5E3);

  int lora_init_count = 0;
  while (!LoRa.begin(433920000)) {
    Serial.println(".");
    lora_init_count++;
    if (lora_init_count >= 30) break;
    delay(500);
  }
  Serial.println("LoRa Initialised");
}

// ===================== Main Loop =====================
void loop() {
  // --- Receive LoRa packets ---
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String LoRaData = LoRa.readString();
    Serial.print("Received packet: ");
    Serial.println(LoRaData);

    // Expected format: "10320111" or "10320100" (deviceID = 103201)
    String deviceid = LoRaData.substring(0, 6);
    String devicestatus = LoRaData.substring(6, 8);

    Serial.print("Device ID: "); Serial.println(deviceid);
    Serial.print("Status: "); Serial.println(devicestatus);

    // --- Motor ON command ---
    if (deviceid.equals("103201") && devicestatus.equals("11")) {
      display.clear();
      display.setSegments(seg_full);
      delay(500);
      tcount1++;
      if (tcount1 >= 4) {                // debounce – require 4 packets
        setMotor(true);                  // starts only if water present
        tcount1 = 0;
        sensor_status = 0;               // assume tank full after start?
      }
    }
    // --- Motor OFF command ---
    else if (deviceid.equals("103201") && devicestatus.equals("00")) {
      tcount2++;
      display.clear();
      display.setSegments(seg_empty);
      if (tcount2 >= 4) {
        setMotor(false);
        tcount2 = 0;
        sensor_status = 1;
      }
    }
    // --- Tank no‑data (from remote level sensor) ---
    else if (deviceid.equals("2010") && devicestatus.equals("22")) {
      tcount3++;
      if (tcount3 >= 2) {
        Serial.println("Tank No Data.........");
        tcount3 = 0;
        sensor_status = 2;
      }
    } else {
      // reset counters if wrong device/status
      tcount1 = 0;
      tcount2 = 0;
      tcount3 = 0;
    }

    Serial.print("RSSI: "); Serial.println(LoRa.packetRssi());
  }

  // --- Debug output ---
  Serial.println("=========================================");
  Serial.print("Motor Status: "); Serial.println(motor_status);
  Serial.print("Motor Duration (min): "); Serial.println(motor_duration);
  Serial.print("Motor Time (sec left): "); Serial.println(motor_time);
  Serial.print("Tank Status: "); Serial.println(sensor_status);
  Serial.print("Water present: "); Serial.println(isWaterPresent() ? "YES" : "NO");

  // --- Manual button handling (toggle motor with water check) ---
  if (digitalRead(MANUAL_BUTTON) == LOW) {
    delay(50);   // debounce
    if (motor_status == 0) {
      setMotor(true);    // checks water internally
    } else {
      setMotor(false);
    }
    delay(200);  // extra delay to avoid multiple toggles
  }

  // --- Motor countdown and auto-stop when tank full (sensor_status == 0) ---
  if (motor_status == 1) {
    motor_time -= 1;
    // Display: first digit = 1 (running), then remaining minutes+1
    display.showNumberDec(1, false, 1, 0);
    display.showNumberDec((motor_time / 60) + 1, false);

    // Stop if duration expired or tank becomes full
    if (motor_time <= 0 || sensor_status == 0) {
      temp_count1++;
      if (temp_count1 >= 5) {
        display.clear();
        setMotor(false);
        temp_count1 = 0;
        display.showNumberDec(0, false);
        motor_time = motor_duration * 60;   // reset for next run
      }
    } else {
      temp_count1 = 0;
    }
    delay(500);
  } else {
    // motor off – just show 0
    display.showNumberDec(0, false);
    delay(500);
  }

  count++;
  delay(500);
}