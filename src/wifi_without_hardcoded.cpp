#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <EEPROM.h>

#define BUTTON_PIN D9         
#define EEPROM_SIZE 96         
#define MAX_SSID_LEN 32
#define MAX_PASS_LEN 64

char ssid[MAX_SSID_LEN + 1];
char password[MAX_PASS_LEN + 1];


void saveCredentials(const char* ssid, const char* password) {
  EEPROM.begin(EEPROM_SIZE);
  Serial.println("Saving credentials to EEPROM...");

  for (int i = 0; i < EEPROM_SIZE; ++i) {
    EEPROM.write(i, 0);
  }

  
  for (int i = 0; i < MAX_SSID_LEN && ssid[i] != '\0'; ++i) {
    EEPROM.write(i, ssid[i]);
  }

  for (int i = 0; i < MAX_PASS_LEN && password[i] != '\0'; ++i) {
    EEPROM.write(MAX_SSID_LEN + i, password[i]);
  }

  EEPROM.commit();
  EEPROM.end();
  Serial.println("Credentials saved.");
}


void loadCredentials() {
  EEPROM.begin(EEPROM_SIZE);

  for (int i = 0; i < MAX_SSID_LEN; ++i) {
    ssid[i] = EEPROM.read(i);
  }
  ssid[MAX_SSID_LEN] = '\0';

  for (int i = 0; i < MAX_PASS_LEN; ++i) {
    password[i] = EEPROM.read(MAX_SSID_LEN + i);
  }
  password[MAX_PASS_LEN] = '\0';

  EEPROM.end();
}


void printCredentials() {
  Serial.print("Stored SSID: ");
  Serial.println(ssid);
  Serial.print("Stored Password: ");
  Serial.println(password);
}


void connectToStoredWiFi() {
  Serial.print("Connecting to SSID: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();
  const unsigned long timeout = 15000;

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < timeout) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect. Rebooting...");
    delay(2000);
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);    
  pinMode(LED_BUILTIN, OUTPUT);         

  delay(1000); 

  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("Button pressed - Starting config portal...");

    WiFiManager wifiManager;

    
    wifiManager.setSaveConfigCallback([]() {
      Serial.println("WiFiManager: Save config callback triggered");
      saveCredentials(WiFi.SSID().c_str(), WiFi.psk().c_str());
    });

   
    if (!wifiManager.startConfigPortal("SetupWiFi", "admin123")) {
      Serial.println("Failed to connect through config portal. Rebooting...");
      delay(2000);
      ESP.restart();
    }

    Serial.println("Connected via config portal.");
  } else {
    
    loadCredentials();
    printCredentials(); 
    connectToStoredWiFi();
  }
}

void loop() {
  
  digitalWrite(LED_BUILTIN, LOW); 
  delay(500);
  digitalWrite(LED_BUILTIN, HIGH); 
  delay(500);
}
