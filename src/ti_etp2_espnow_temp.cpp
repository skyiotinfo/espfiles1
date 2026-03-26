#include <Arduino.h>
#include <espnow.h>
#include <ESP8266WiFi.h>

#define M3_PIN D6
// #define M4_PIN D7
#define FLOAT_SENSOR D1
#define UT_SENSOR D2
#define TANK2_PIN D8

uint8_t receiverMac[] = {0xD8, 0xBF, 0xC0, 0xFD, 0x74, 0x1D};
unsigned long motorStartTime = 0;
bool motorRunning = false;
const unsigned long MAX_RUNTIME = 120UL * 60UL * 1000UL;
bool lastTankState = true;
int cycleState = 0;
bool motorAckReceived = false;
static unsigned long lastRead = 0;
static unsigned long lastUTCheck = 0;
uint8_t requestValue = 0;
bool requestActive = false;

unsigned long tank2StartTime = 0;
bool tank2Running = false;
const unsigned long TANK2_MAX_RUNTIME = 20UL * 60UL * 1000UL; // 20 min

void setupPins();
void setupESPNOW();
void controlLogic();
void sendESPNow(uint8_t value);
void startM3();
void startM4();
void stopMotors();

void onSent(uint8_t *mac_addr, uint8_t sendStatus)
{

  Serial.print("Delivery Status: ");

  if (sendStatus == 0)
    Serial.println("Success");
  else
    Serial.println("Fail");
}

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len)
{
  uint8_t value = data[0];

  Serial.print("ACK Received: ");
  Serial.println(value);

  if (value == 2)
  {
    Serial.println("Motor Start ACK received");

    motorAckReceived = true;
    requestActive = false;
  }
}

void setup()
{

  Serial.begin(115200);

  setupPins();
  setupESPNOW();
  Serial.println("System Ready");
}
void loop()
{
  controlLogic();

  if (requestActive && !motorAckReceived)
  {
    static unsigned long lastSendTime = 0;
    const unsigned long SEND_INTERVAL = 200;

    if (millis() - lastSendTime >= SEND_INTERVAL)
    {
      lastSendTime = millis();

      uint8_t result = esp_now_send(receiverMac, &requestValue, sizeof(requestValue));

      Serial.print("ESP-NOW Sent: ");
      Serial.println(requestValue);

      if (result == 0)
        Serial.println("Send request OK");
      else
        Serial.println("Send request ERROR");
    }
  }

  delay(5);
}

void setupPins()
{

  pinMode(M3_PIN, OUTPUT);
  // pinMode(M4_PIN, OUTPUT);
  pinMode(TANK2_PIN, OUTPUT);
  pinMode(FLOAT_SENSOR, INPUT_PULLUP);
  pinMode(UT_SENSOR, INPUT_PULLUP);

  stopMotors();
  digitalWrite(TANK2_PIN, LOW);
}

void setupESPNOW()
{

  WiFi.mode(WIFI_STA);

  if (esp_now_init() != 0)
  {
    Serial.println("ESP-NOW Init Failed");
    return;
  }

  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_add_peer(receiverMac, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);

  Serial.println("ESP-NOW Ready");
}

bool isUTLowConfirmed()
{
  const int checks = 5;
  int lowCount = 0;

  for (int i = 0; i < checks; i++)
  {
    if (digitalRead(UT_SENSOR) == LOW)
    {
      lowCount++;
    }
    delay(2); 
  }

  return (lowCount >= 4); 
}

void controlLogic()
{

  static bool tankFull;

  if (millis() - lastRead > 200)
  {
    tankFull = digitalRead(FLOAT_SENSOR);
    lastRead = millis();
  }

  if (millis() - lastUTCheck > 100)
  {
    lastUTCheck = millis();

    if (isUTLowConfirmed())
    {
      Serial.println("UT LOW → Emergency");

      stopMotors();
      digitalWrite(TANK2_PIN, LOW);
      tank2Running = false;

      sendESPNow(1);
      cycleState = 0;
      return;
    }
  }

  if (tankFull != lastTankState)
  {
    if (!tankFull) 
    {
      Serial.println("Tank EMPTY");

      if (cycleState == 0)
      {
        sendESPNow(1);
        cycleState = 1;
      }
      else if (cycleState == 2)
      {
        Serial.println("Stop M3 → Start Tank2");

        digitalWrite(M3_PIN, LOW);

        Serial.println("Tank2 ON");
        digitalWrite(TANK2_PIN, HIGH);

        tank2StartTime = millis();
        tank2Running = true;

        cycleState = 3;
      }
      else if (cycleState == 4)
      {
        Serial.println("Stop M3 → Send ESP");

        digitalWrite(M3_PIN, LOW);
        sendESPNow(1);

        cycleState = 0;
      }
    }
    else 
    {
      Serial.println("Tank FULL");

      if (cycleState == 1)
      {
        sendESPNow(0);

        startM3();
        cycleState = 2;
      }
      else if (cycleState == 3)
      {
        Serial.println("Stop Tank2 → Start M3");

        digitalWrite(TANK2_PIN, LOW);
        tank2Running = false;

        startM3();
        cycleState = 4;
      }
    }

    lastTankState = tankFull;
  }

  if (tank2Running && digitalRead(TANK2_PIN) == HIGH)
  {
    if (millis() - tank2StartTime > TANK2_MAX_RUNTIME)
    {
      Serial.println("Tank2 Timeout → Switching to M3");

      digitalWrite(TANK2_PIN, LOW);
      tank2Running = false;

      startM3();
      cycleState = 4;
    }
  }
}

void sendESPNow(uint8_t value)
{
  requestValue = value;
  requestActive = true;
  motorAckReceived = false;
}

void startM3()
{

  Serial.println("Starting M3");

  digitalWrite(M3_PIN, HIGH);
  // digitalWrite(M4_PIN, LOW);
}

void startM4()
{

  Serial.println("Starting M4");

  // digitalWrite(M4_PIN, HIGH);
  digitalWrite(M3_PIN, LOW);
}

void stopMotors()
{

  digitalWrite(M3_PIN, LOW);
  // digitalWrite(M4_PIN, LOW);
}
