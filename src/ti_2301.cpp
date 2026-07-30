#include <LoRa.h>
#include <SPI.h>
//#include <ESP8266WiFi.h>

#define ss 15
#define rst 16
#define dio0 2
#define networkid "1023"
#define deviceid "01"
 
int counter = 10;
const int hsen = D2;
const int lsen = D9;
const int hpin = D1;
const int sled = LED_BUILTIN;
int value = 11;
int state=0;
int vstate1=2;
int vstate2=2;
int volt_state=1;
int temp_count1=0;
int temp_count2=0;
int temp_count3=0;
int rnum = 3;
int rnum1 = 1;
 
void setup() 
{
  Serial.begin(115200); 
  pinMode(D0, WAKEUP_PULLUP);
  pinMode(hsen, INPUT_PULLUP); 
  pinMode(lsen, INPUT_PULLUP);
  pinMode(hpin, OUTPUT);
  pinMode(sled, OUTPUT);
  digitalWrite(hpin,LOW);
  digitalWrite(sled,HIGH);
  while (!Serial);
  Serial.println("LoRa Sender");
  LoRa.setPins(ss, rst, dio0);    //setup LoRa transceiver module
  LoRa.setSyncWord(0xA2);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(62.5E3);
  while (!LoRa.begin(433920000))     //433E6 - Asia, 866E6 - Europe, 915E6 - North America
  {
    Serial.println(".");
    delay(500);
  }
  Serial.println("LoRa Initializing OK!");
}

void send_data(){
  int i;
  for(i=0;i<=(10);i++){
    LoRa.beginPacket();   //Send LoRa packet to receiver
    LoRa.print(networkid);
    LoRa.print(deviceid);
    LoRa.print(vstate1);
    LoRa.print(vstate2);
    LoRa.endPacket(); 
    Serial.print(".");
    Serial.print(networkid);
    Serial.print(deviceid);
    Serial.print(vstate1);
    Serial.print(vstate2);
    delay(100);
  }
  Serial.println("");
}
 
void loop() 
{

  Serial.print("Sending packet: ");
  Serial.println(counter);

  if(digitalRead(hsen)==0 && digitalRead(hpin)==1){
    temp_count1=temp_count1+1;
    Serial.println("temp count");
    Serial.println(temp_count1);
    if(temp_count1>=3){
      state=2;
      vstate1=0;
      vstate2=0;
    }
    if(temp_count1>=15){
      Serial.println("LOW....");
      digitalWrite(hpin, LOW);
      temp_count1=0;
    }
  }else{
    temp_count1=0;
  }

  if(digitalRead(lsen)==0 && digitalRead(hpin)==0){
    temp_count2=temp_count2+1;
    Serial.println("temp count");
    Serial.println(temp_count2);
    if(temp_count2>=3){
      Serial.println("HIGH...");
      digitalWrite(hpin, HIGH);
      state=1;
      vstate1=1;
      vstate2=1;
      temp_count2=0;
    }
  }else{
    temp_count2=0;
  }

  //=======================

  if(digitalRead(hpin)==1 && digitalRead(lsen)!=0){
    temp_count3=temp_count3+1;
    if(temp_count3>=3){
      state=2;
      vstate1=0;
      vstate2=0;
    }
    if(temp_count3>=10){
      digitalWrite(hpin, LOW);
      temp_count3=0;
    }
  }else{
    temp_count3=0;
  }

  counter++;
  if(counter>=200){
    counter=10;
  }
  send_data();
  digitalWrite(sled,LOW);
  delay(50);
  digitalWrite(sled,HIGH);
  delay(50);
  delay(2000);
}
