#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

const char* ssid = "Anupam";
const char* password = "12345678";
const char* serverName = "https://esp-02.asia-southeast1.firebasedatabase.app/users/uid/10103.json";
int sdevice[8] = {0};
int prev_sdevice[8] = {0};
String etime[8];
String ftime[8];

char motor_st = 'S';
const int mt_st = D8;
int mt_status = 0;

const int sled = D7;

unsigned long mt_last_active_time = 0;
const unsigned long MT_TIMEOUT = 5 * 60 * 1000; 

void sendDataToFirebase();
void checkWiFiConnection();
String getCurrentTime();
void readMtStFromFirebase();

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi.");
  }

  configTime(19800, 0, "pool.ntp.org", "time.nist.gov");

  for (int i = 0; i < 8; i++) {
    etime[i] = "0000";
    ftime[i] = "0000";
  }

  pinMode(mt_st, OUTPUT);
  digitalWrite(mt_st, mt_status);

  pinMode(sled, OUTPUT);
  digitalWrite(sled, LOW);

  if (mt_status == 1) {
    mt_last_active_time = millis();
  }
}

void loop() {
  checkWiFiConnection();
 

  if (Serial.available()) {
    String received = Serial.readStringUntil('\n');
    received.trim();

    if (received.length() == 9) {
      bool stateChanged = false;

      for (int i = 0; i < 8; i++) {
        char c = received.charAt(i);
        if (c >= '0' && c <= '9') {
          int newVal = c - '0';
          if (newVal != sdevice[i]) {
            prev_sdevice[i] = sdevice[i];
            sdevice[i] = newVal;
            String currentTime = getCurrentTime();
            if (newVal == 1) etime[i] = currentTime;
            else ftime[i] = currentTime;
            stateChanged = true;
          }
        }
      }

      char newStatus = received.charAt(8);
      if (newStatus != motor_st) {
        motor_st = newStatus;
        stateChanged = true;
      }

      if (stateChanged) {
        Serial.print("State changed. motor_st: ");
        Serial.println(motor_st);
        sendDataToFirebase();
      } else {
        Serial.print("no change");
         readMtStFromFirebase();
      }
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(sled, HIGH);
    delay(2000);
     digitalWrite(sled, LOW);
    delay(2000);
  
  }

  if (mt_status == 1 && millis() - mt_last_active_time > MT_TIMEOUT) {
    Serial.println("Motor timeout reached. Turning off mt_status.");

    mt_status = 0;
    digitalWrite(mt_st, mt_status);
    motor_st = 'S'; 

    sendDataToFirebase(); 
  }

  delay(200);
}

void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Reconnecting...");
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
      delay(500);
      Serial.print(".");
      attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nReconnected to WiFi.");
    } else {
      Serial.println("\nReconnection failed.");
    }
  }
}

String getCurrentTime() {
  time_t now = time(nullptr);
  struct tm* p_tm = localtime(&now);
  char timeStr[5];
  if (p_tm) {
    snprintf(timeStr, sizeof(timeStr), "%02d%02d", p_tm->tm_hour, p_tm->tm_min);
    return String(timeStr);
  } else {
    return "0000";
  }
}

void sendDataToFirebase() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;
    https.begin(client, serverName);
    https.addHeader("Content-Type", "application/json");

    String jsonData = "{";
    jsonData += "\"motor_st\":\"" + String(motor_st) + "\",";
    jsonData += "\"mt_status\":\"" + String(mt_status) + "\",";
    jsonData += "\"sdevice\":[";

    for (int i = 0; i < 8; i++) {
      jsonData += "{";
      jsonData += "\"" + String(i) + "\":\"" + String(sdevice[i]) + "\",";
      jsonData += "\"etime\":\"" + etime[i] + "\",";
      jsonData += "\"ftime\":\"" + ftime[i] + "\"";
      jsonData += "}";
      if (i < 7) jsonData += ",";
    }

    jsonData += "]}";

    Serial.println("Sending to Firebase: " + jsonData);
    int httpResponseCode = https.PUT(jsonData);

    if (httpResponseCode > 0) {
      Serial.print("Firebase response code: ");
      Serial.println(httpResponseCode);
      Serial.println(https.getString());
    } else {
      Serial.print("Error sending to Firebase: ");
      Serial.println(httpResponseCode);
    }

    https.end();
  } else {
    Serial.println("WiFi not connected. Cannot send data.");
  }
}

void readMtStFromFirebase() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;
    https.begin(client, serverName);
    int httpCode = https.GET();

    if (httpCode > 0) {
      String payload = https.getString();
      Serial.println("Firebase GET: " + payload);

      int index = payload.indexOf("\"mt_status\":");
      if (index != -1) {
        int startIndex = index + 12;
        int endIndex = payload.indexOf(",", startIndex);
        if (endIndex == -1) {
          endIndex = payload.indexOf("}", startIndex);
        }

        String mt_value_str = payload.substring(startIndex, endIndex);
        mt_value_str.replace("\"", "");
        mt_value_str.trim();

        Serial.print("Parsed mt_status value: ");
        Serial.println(mt_value_str);

        int new_mt_status = mt_value_str.toInt();

        digitalWrite(mt_st, new_mt_status);

        if (new_mt_status != mt_status) {
          mt_status = new_mt_status;
          Serial.print("Updated mt_status pin to: ");
          Serial.println(mt_status);

          char newMotorState = (mt_status == 1) ? 'R' : 'S';
          if (motor_st != newMotorState) {
            motor_st = newMotorState;
            Serial.print("Updated motor_st to: ");
            Serial.println(motor_st);
            sendDataToFirebase();
          }

          if (mt_status == 1) {
            mt_last_active_time = millis();
          }
        }
      } else {
        Serial.println("mt_status field not found.");
      }
    } else {
      Serial.print("Firebase GET failed, error: ");
      Serial.println(httpCode);
    }

    https.end();
  }
}
