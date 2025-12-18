#include <EEPROM.h>
#include <Arduino.h>

const int EEPROM_SIZE = 512;
String read_eeprom_data(int val, int len, String read_data);

void writeInt(int address, int value);
void writeString(int address, String data);

int readInt(int address);
String readString(int address);
int temp_cnt=1234;

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  pinMode(D1, INPUT_PULLUP);
  writeInt(0, 0);
  //writeString(10, "Hello Disha");

  Serial.println(readInt(0));
  Serial.println(readString(10));
}

void loop() {
    readInt(0);
    Serial.println(readInt(0));
    Serial.println(readString(10));
    Serial.println(readString(15));
    Serial.println(read_eeprom_data(0, 10, "m_status"));
    if(digitalRead(D1)==0) { // avoid WDT reset
        writeString(10, "BUTTON"+String(temp_cnt));
        temp_cnt++;
    }
    delay(1000);

}

String read_eeprom_data(int val, int len, String read_data) {
    if(read_data  == "m_status") {
        printf("Get motor status\n");
        return "ON";
    }
    else if (read_data  == "sch1") {
        printf("Get Sch1\n");
        return "sch1";
    }
    else if (read_data  == "sch2") {
        printf("Get Sch2\n");
        return "sch2";
    }
    else if (read_data  == "sch3") {
        printf("Get Sch3\n");
        return "sch3";
    }
    return "d1";
}


void writeInt(int address, int value) {
  EEPROM.put(address, value);
  EEPROM.commit();
}

int readInt(int address) {
  int value;
  EEPROM.get(address, value);
  return value;
}

void writeString(int address, String data) {
    for (int i = 0; i < data.length(); i++)
    EEPROM.write(address + i, data[i]);
    EEPROM.write(address + data.length(), '\0');
    EEPROM.commit();
    Serial.println("Written String: " + data);
}

String readString(int address) {
  String data = "";
  char ch;
  while (true) {
    ch = EEPROM.read(address++);
    if (ch == '\0') break;
    data += ch;
  }
  return data;
}

/*

void GetTime() {
    client.setInsecure();
    https.begin(client, gettime_url);
    // Add your headers
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImZrZ2ZkZ3dwcXFmeGhueXV3dHdlIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjAzMzQzNzQsImV4cCI6MjA3NTkxMDM3NH0.Dn805WO5wyPa25yD5fYYcCzB4TgDbnTCb4zBuCiczZU");
    int httpCode = https.GET();
    Serial.print("Response Code: ");
    if(httpCode==200){
      Serial.println(httpCode);
      dt_payload = https.getString();
      updateRTCfromJSON(dt_payload);
      Serial.println("RAW:");
      Serial.println(dt_payload);
      } else {
      Serial.println("Error on HTTP request");  
    }
    https.end();
} 
*/