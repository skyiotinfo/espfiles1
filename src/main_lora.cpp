#include <Arduino.h>
#include <EEPROM.h>
#include <LoRa.h>
#include <SPI.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#define DEBUG 1

#if DEBUG
#define DBG(x) Serial1.println(x)
#define DBGL(x) Serial1.print(x)
#else
#define DBG(x)
#define DBGL(x)
#endif
#define EEPROM_SIZE 256
#define EEPROM_START_UNIX_ADDR 0
#define EEPROM_STOP_UNIX_ADDR 4
#define EEPROM_START_UNIX1_ADDR 8
#define EEPROM_STOP_UNIX1_ADDR 12

#define MOTOR1_PIN D2
#define LORA_STATUS_LED LED_BUILTIN
#define OT_SENSOR_PIN D1
#define MOTOR1_BUTTON D9

#define OT_ACTIVE_LEVEL LOW
int ot_sensorcount = 0;
const int OT_TRIP_COUNT = 3;

bool manualMotor1On = false;
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

struct Schedule
{
    time_t unixTime;
    bool triggered;
};

uint8_t scheduleCount = 3;

struct sch
{
    uint32_t startUnix;
    uint32_t stopUnix;
    uint8_t startTime;
    uint8_t stopTime;
    bool active;
    int state;
    int ack;
    int sch1_en;
};

struct sche
{
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

// ========== Serial Communication Protocol ==========
#define SERIAL_BAUD 115200
#define CMD_START_BYTE 0xAA
#define FB_START_BYTE 0xBB
#define CMD_ID_COMMAND 0x01
#define CMD_ID_FEEDBACK 0x02

// Packed structures for binary transfer
typedef struct __attribute__((packed))
{
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
} serial_command_t;

typedef struct __attribute__((packed))
{
    int motor1PhysicalState;
    int motor1DeviceState;
    uint8_t motor1Cancellation;
    int motor2State;
    int motor2DeviceState;
    uint8_t motor2Cancellation;
    int heartBeatCount;
} serial_feedback_t;

serial_command_t receivedCommand;
serial_feedback_t feedbackData;

// Serial receive buffer
#define SERIAL_BUFFER_SIZE 64
uint8_t serialBuffer[SERIAL_BUFFER_SIZE];
uint8_t serialIndex = 0;
bool receivingPacket = false;
uint8_t expectedLength = 0;
uint8_t commandId = 0;

// ====================================================

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
void processSerialCommand();
void sendFeedbackToDevice2();
uint8_t calculateChecksum(uint8_t *data, size_t len);
void sendSerialPacket(uint8_t startByte, uint8_t cmdId, uint8_t *payload, size_t payloadLen);

uint32_t getCurrentUnix()
{
    return baseUnixTime + ((millis() - baseMillis) / 1000);
}



void processSerialCommand()
{
    baseUnixTime = receivedCommand.currentUnixTime;
    baseMillis = millis();

    DBG("Received data from Device 2 via Serial:");
    DBGL("Current Time: ");
    DBG(receivedCommand.currentUnixTime);
    DBGL("Motor1 App State: ");
    DBG(receivedCommand.motor1AppState);
    DBGL("Motor1 Start: ");
    DBG(receivedCommand.motor1StartUnix);
    DBGL("Motor1 Stop: ");
    DBG(receivedCommand.motor1StopUnix);

    if (!sch1.active)
    {
        if (receivedCommand.motor1AppState == 1 && digitalRead(MOTOR1_PIN) == LOW)
        {
            digitalWrite(MOTOR1_PIN, HIGH);
            DBG("Motor1 ON from App via Serial");
            feedbackData.motor1PhysicalState = 1;
            feedbackData.motor1DeviceState = 1;
            manualMotor1On = true;
            scheduleCancelledByApp = false;
        }
        else if (receivedCommand.motor1AppState == 0 && digitalRead(MOTOR1_PIN) == HIGH)
        {
            digitalWrite(MOTOR1_PIN, LOW);
            DBG("Motor1 OFF from App via Serial");
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
        sch1.stopUnix != receivedCommand.motor1StopUnix)
    {
        sch1.startUnix = receivedCommand.motor1StartUnix;
        sch1.stopUnix = receivedCommand.motor1StopUnix;
        saveScheduleToEEPROM();
    }

    bool appOffPressed = (lastAppState == 1 && receivedCommand.motor1AppState == 0);
    lastAppState = receivedCommand.motor1AppState;

    if (sch1.active && appOffPressed)
    {
        sch1.active = false;
        scheduleCancelledByApp = true;
        digitalWrite(MOTOR1_PIN, LOW);
        feedbackData.motor1Cancellation = true;
        DBG("Motor1 Schedule cancelled by App via Serial");
    }

    if (scheduleCancelledByApp && receivedCommand.motor1StartUnix > receivedCommand.currentUnixTime)
    {
        scheduleCancelledByApp = false;
        DBG("Motor1: New schedule detected - cancellation cleared");
    }

    updated_sche1.state1 = receivedCommand.motor2AppState;
    sche1.ack1 = receivedCommand.motor2Ack;
    sche1.sch1_en1 = receivedCommand.motor2SchEn;

    if (sche1.startUnix1 != receivedCommand.motor2StartUnix ||
        sche1.stopUnix1 != receivedCommand.motor2StopUnix)
    {
        sche1.startUnix1 = receivedCommand.motor2StartUnix;
        sche1.stopUnix1 = receivedCommand.motor2StopUnix;
        saveScheduleToEEPROM1();
    }

    if (sche1.state1 != receivedCommand.motor2AppState)
    {
        sche1.state1 = receivedCommand.motor2AppState;

        if (sche1.state1 == 1)
        {
            loraVstate1 = 0;
            loraVstate2 = 0;
            feedbackData.motor2State = 1;
            feedbackData.motor2DeviceState = 1;
            DBG("Motor2 App ON via Serial");
        }
        else if (sche1.state1 == 0)
        {
            loraVstate1 = 1;
            loraVstate2 = 1;
            feedbackData.motor2State = 0;
            feedbackData.motor2DeviceState = 0;
            DBG("Motor2 App OFF via Serial");
        }

        sendLORAData();
    }

    bool appOffPressed1 = (lastAppState1 == 1 && receivedCommand.motor2AppState == 0);
    lastAppState1 = receivedCommand.motor2AppState;

    if (sche1.active1 && appOffPressed1)
    {
        sche1.active1 = false;
        scheduleCancelledByApp1 = true;
        sche1.sendPostScheduleFull = true;
        sche1.postScheduleEndTime = millis() + POST_SCHEDULE_LORA_TIME;
        loraVstate1 = 1;
        loraVstate2 = 1;
        feedbackData.motor2Cancellation = true;
        DBG("Motor2 Schedule cancelled by App via Serial");
    }

    if (scheduleCancelledByApp1 && receivedCommand.motor2StartUnix > receivedCommand.currentUnixTime)
    {
        scheduleCancelledByApp1 = false;
        DBG("Motor2: New schedule detected - cancellation cleared");
    }

    sendFeedbackToDevice2();
}

void sendFeedbackToDevice2()
{
    feedbackData.motor1PhysicalState = digitalRead(MOTOR1_PIN) == HIGH ? 1 : 0;
    feedbackData.heartBeatCount = 10;

    sendSerialPacket(FB_START_BYTE, CMD_ID_FEEDBACK, (uint8_t *)&feedbackData, sizeof(feedbackData));
    DBG("Feedback sent to Device 2 via Serial");
}

// ========== Serial Packet Handling ==========
uint8_t calculateChecksum(uint8_t *data, size_t len)
{
    uint8_t cs = 0;
    for (size_t i = 0; i < len; i++)
    {
        cs ^= data[i];
    }
    return cs;
}

void sendSerialPacket(uint8_t startByte, uint8_t cmdId, uint8_t *payload, size_t payloadLen)
{
    Serial.write(startByte);
    Serial.write(cmdId);
    Serial.write((uint8_t)payloadLen);
    Serial.write(payload, payloadLen);
    uint8_t checksum = calculateChecksum(payload, payloadLen);
    // Include start, cmdId, length in checksum? We'll just checksum payload for simplicity.
    Serial.write(checksum);
}

void handleSerialByte(uint8_t c)
{
    static unsigned long lastByteTime = 0;

    if (millis() - lastByteTime > 100)
    {
        receivingPacket = false;
        serialIndex = 0;
    }
    lastByteTime = millis();

    if (!receivingPacket)
    {
        if (c == CMD_START_BYTE || c == FB_START_BYTE)
        {
            receivingPacket = true;
            serialIndex = 0;
            serialBuffer[serialIndex++] = c;
        }
        return;
    }

    // Prevent overflow
    if (serialIndex >= SERIAL_BUFFER_SIZE)
    {
        receivingPacket = false;
        serialIndex = 0;
        return;
    }

    serialBuffer[serialIndex++] = c;

    if (serialIndex == 2)
    {
        commandId = c;
    }

    else if (serialIndex == 3)
    {
        expectedLength = c;

        // Sanity check
        if (expectedLength > 50)
        {
            receivingPacket = false;
            serialIndex = 0;
            return;
        }
    }

    else if (serialIndex == (4 + expectedLength))
    {

        uint8_t receivedChecksum = c;
        uint8_t computedChecksum =
            calculateChecksum(&serialBuffer[3], expectedLength);

        if (computedChecksum == receivedChecksum)
        {

            if (commandId == CMD_ID_COMMAND &&
                expectedLength == sizeof(serial_command_t))
            {

                memcpy(&receivedCommand,
                       &serialBuffer[3],
                       expectedLength);

                processSerialCommand();
            }
        }

        receivingPacket = false;
    }
}

// =============================================

void loadSchedules()
{
    sch1 = {
        1767225600,
        1767225600,
        0,
        0,
        false,
        0,
        0,
        0};
}

void saveScheduleToEEPROM()
{
    if (sch1.startUnix != lastSavedStartUnix ||
        sch1.stopUnix != lastSavedStopUnix)
    {
        EEPROM.put(EEPROM_START_UNIX_ADDR, sch1.startUnix);
        EEPROM.put(EEPROM_STOP_UNIX_ADDR, sch1.stopUnix);
        EEPROM.commit();
        lastSavedStartUnix = sch1.startUnix;
        lastSavedStopUnix = sch1.stopUnix;
        DBG("Motor1 schedule saved to EEPROM");
    }
}

void loadScheduleFromEEPROM()
{
    EEPROM.get(EEPROM_START_UNIX_ADDR, sch1.startUnix);
    EEPROM.get(EEPROM_STOP_UNIX_ADDR, sch1.stopUnix);

    if (sch1.startUnix < 1000000000 || sch1.stopUnix < sch1.startUnix)
    {
        DBG("Invalid EEPROM data for Motor1, using defaults");
        loadSchedules();
        scheduleCancelledByApp = false;
    }

    sch1.active = false;
    lastSavedStartUnix = sch1.startUnix;
    lastSavedStopUnix = sch1.stopUnix;
}

void checkSch(uint32_t nowUnix)
{
    if (manualMotor1On == true)
    {
        if (!sch1.active)
        {
            DBG("MOTOR1 MANUAL ON");
        }
        sch1.active = false;
        digitalWrite(MOTOR1_PIN, HIGH);
        feedbackData.motor1PhysicalState = 1;
        feedbackData.motor1DeviceState = 1;
        return;
    }

    if (scheduleCancelledByApp)
    {
        if (sch1.active)
        {
            DBG("MOTOR1 CANCELLED BY APP");
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
        sch1.sch1_en == 1)
    {
        sch1.active = true;
        digitalWrite(MOTOR1_PIN, HIGH);
        DBG("MOTOR1 SCHEDULE START");
        feedbackData.motor1PhysicalState = 1;
        feedbackData.motor1DeviceState = 1;
    }

    if (sch1.active && nowUnix >= sch1.stopUnix)
    {
        sch1.active = false;
        digitalWrite(MOTOR1_PIN, LOW);
        DBG("MOTOR1 SCHEDULE STOP");
        feedbackData.motor1PhysicalState = 0;
        feedbackData.motor1DeviceState = 0;
    }
}

void loadSchedules1()
{
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
        0};
}

void saveScheduleToEEPROM1()
{
    if (sche1.startUnix1 != lastSavedStartUnix1 ||
        sche1.stopUnix1 != lastSavedStopUnix1)
    {
        EEPROM.put(EEPROM_START_UNIX1_ADDR, sche1.startUnix1);
        EEPROM.put(EEPROM_STOP_UNIX1_ADDR, sche1.stopUnix1);
        EEPROM.commit();
        lastSavedStartUnix1 = sche1.startUnix1;
        lastSavedStopUnix1 = sche1.stopUnix1;
        DBG("Motor2 schedule saved to EEPROM");
    }
}

void loadScheduleFromEEPROM1()
{
    EEPROM.get(EEPROM_START_UNIX1_ADDR, sche1.startUnix1);
    EEPROM.get(EEPROM_STOP_UNIX1_ADDR, sche1.stopUnix1);

    if (sche1.startUnix1 < 1000000000 || sche1.stopUnix1 < sche1.startUnix1)
    {
        DBG("Invalid EEPROM data for Motor2, using defaults");
        loadSchedules1();
        scheduleCancelledByApp1 = false;
    }

    sche1.active1 = false;
    sche1.sendPostScheduleFull = false;
    lastSavedStartUnix1 = sche1.startUnix1;
    lastSavedStopUnix1 = sche1.stopUnix1;
}

void checkSch1(uint32_t nowUnix)
{
    if (!sche1.active1 &&
        !scheduleCancelledByApp1 &&
        nowUnix >= sche1.startUnix1 &&
        nowUnix < sche1.stopUnix1 &&
        sche1.ack1 == 1 &&
        sche1.sch1_en1 == 1)
    {
        sche1.active1 = true;
        sche1.sendPostScheduleFull = false;
        loraVstate1 = 0;
        loraVstate2 = 0;
        feedbackData.motor2State = 1;
        feedbackData.motor2DeviceState = 1;
        DBG("MOTOR2 SCHEDULE START");
    }
    else if (sche1.active1 && nowUnix >= sche1.stopUnix1)
    {
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
        DBG("MOTOR2 SCHEDULE STOP");
    }
}

void initializeLORA()
{
    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
    LoRa.setSyncWord(0xA2);
    LoRa.setTxPower(20);
    LoRa.setSpreadingFactor(12);
    LoRa.setSignalBandwidth(62.5E3);

    if (!LoRa.begin(LORA_FREQUENCY))
    {
        DBG("LORA initialization failed!");
        loraInitialized = false;
        return;
    }

    loraInitialized = true;
    DBG("LORA Initialized OK!");
}

void sendLORAData()
{
    if (!loraInitialized)
    {
        DBG("LORA not initialized!");
        return;
    }

    if (sche1.active1)
    {
        loraVstate1 = 0;
        loraVstate2 = 0;
        DBG("LORA: Sending EMPTY signal (schedule active)");
    }
    else if (sche1.sendPostScheduleFull && millis() < sche1.postScheduleEndTime)
    {
        loraVstate1 = 1;
        loraVstate2 = 1;
        DBG("LORA: Sending FULL signal (post-schedule)");
    }
    else if (sche1.sendPostScheduleFull && millis() >= sche1.postScheduleEndTime)
    {
        sche1.sendPostScheduleFull = false;

        if (sche1.state1 == 1)
        {
            loraVstate1 = 0;
            loraVstate2 = 0;
            DBG("LORA: Sending EMPTY signal (app ON)");
        }
        else
        {
            loraVstate1 = 1;
            loraVstate2 = 1;
            DBG("LORA: Sending FULL signal (app OFF)");
        }
    }
    else
    {
        if (sche1.state1 == 1)
        {
            loraVstate1 = 0;
            loraVstate2 = 0;
            DBG("LORA: Sending EMPTY signal (app ON)");
        }
        else
        {
            loraVstate1 = 1;
            loraVstate2 = 1;
            DBG("LORA: Sending FULL signal (app OFF)");
        }
    }

    for (int i = 0; i <= (5); i++)
    {
        LoRa.beginPacket();
        LoRa.print(loraDeviceId);
        LoRa.print(loraVstate1);
        LoRa.print(loraVstate2);
        LoRa.endPacket();
        DBGL(".");
        DBGL(loraDeviceId);
        DBGL(loraVstate1);
        DBGL(loraVstate2);
        delay(50);
    }
}

void process_LocalEvents()
{
    static int lastDay = -1;
    uint32_t nowUnix = getCurrentUnix();

    if (lastDay == -1)
        lastDay = nowUnix / 86400;

    if ((nowUnix / 86400) != lastDay)
    {
        scheduleCancelledByApp = false;
        lastDay = nowUnix / 86400;
        DBG("Motor1: New day – app cancellation reset");
    }

    if (digitalRead(MOTOR1_BUTTON) == LOW)
    {
        delay(50);
        if (digitalRead(MOTOR1_BUTTON) == LOW)
        {
            DBG("Motor1 Button Pressed");

            if (sch1.active && digitalRead(MOTOR1_PIN) == HIGH)
            {
                sch1.active = false;
                scheduleCancelledByApp = true;
                feedbackData.motor1Cancellation = true;
                DBG("Motor1 Schedule cancelled by Manual Button");
            }

            if (digitalRead(MOTOR1_PIN) == HIGH)
            {
                digitalWrite(MOTOR1_PIN, LOW);
                manualMotor1On = false;
                feedbackData.motor1PhysicalState = 0;
                feedbackData.motor1DeviceState = 0;
                DBG("Motor1 OFF by Button");
            }
            else
            {
                digitalWrite(MOTOR1_PIN, HIGH);
                manualMotor1On = true;
                feedbackData.motor1PhysicalState = 1;
                feedbackData.motor1DeviceState = 1;
                DBG("Motor1 ON by Button");
            }

            sendFeedbackToDevice2();
            delay(500);
        }
    }

    if (mili_now - lastExecutionTime >= EXECUTION_INTERVAL)
    {
        DBG("\n=== Processing Motor1 Events ===");
        lastExecutionTime = mili_now;

        checkSch(nowUnix);

        DBGL("Current Unix Time (from Device 2): ");
        DBG(nowUnix);

        feedbackData.heartBeatCount = 10;
        sendFeedbackToDevice2();
    }
}

void process_LoraEvents()
{
    static int lastDay1 = -1;
    uint32_t nowUnix = getCurrentUnix();

    if (lastDay1 == -1)
        lastDay1 = nowUnix / 86400;

    if ((nowUnix / 86400) != lastDay1)
    {
        scheduleCancelledByApp1 = false;
        lastDay1 = nowUnix / 86400;
        DBG("Motor2: New day - app cancellation reset");
    }

    checkSch1(nowUnix);

    if (mili_now - lastLoraExecutionTime >= LORA_INTERVAL)
    {
        DBG("\n=== Processing Motor2 LORA Events ===");
        lastLoraExecutionTime = mili_now;

        DBGL("Current Time (from Device 2): ");
        DBG(nowUnix);

        sendLORAData();
    }
}

void setup()
{
    Serial1.begin(115200);
    Serial.begin(SERIAL_BAUD);
    Serial.setTimeout(5);
    EEPROM.begin(EEPROM_SIZE);
    pinMode(MOTOR1_PIN, OUTPUT);
    digitalWrite(MOTOR1_PIN, LOW);
    pinMode(MOTOR1_BUTTON, INPUT_PULLUP);
    pinMode(LORA_STATUS_LED, OUTPUT);
    pinMode(OT_SENSOR_PIN, INPUT_PULLUP);

    loadScheduleFromEEPROM();
    loadScheduleFromEEPROM1();

    initializeLORA();

    feedbackData.motor1PhysicalState = 0;
    feedbackData.motor1DeviceState = 0;
    feedbackData.motor1Cancellation = false;
    feedbackData.motor2State = 0;
    feedbackData.motor2DeviceState = 0;
    feedbackData.motor2Cancellation = false;
    feedbackData.heartBeatCount = 0;

    DBG("Device 1 (Motor+LORA) setup complete");
}

void loop()
{
    mili_now = millis();

    // Handle incoming serial data
    while (Serial.available())
    {
        uint8_t c = Serial.read();
        handleSerialByte(c);
    }

    process_LocalEvents();
    process_LoraEvents();

    delay(100);
}