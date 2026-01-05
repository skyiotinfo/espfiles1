#include <LoRa.h>
#include <SPI.h>

#define ss 15
#define rst 16
#define dio0 2
#define networkid "1031"
#define deviceid "02"
 
int counter = 1;
const int hsen = D1;
const int lsen = D2;
const int spin = LED_BUILTIN;

int value = 11;
int state=0;
int vstate1=2;
int vstate2=2;
int volt_state=1;
int temp_count1=0;
int temp_count2=0;
int temp_count3=0;

 
void setup() 
{
  Serial.begin(115200); 
  pinMode(D0, WAKEUP_PULLUP);
  pinMode(hsen, INPUT_PULLUP); 
  pinMode(lsen, INPUT_PULLUP);
  pinMode(spin, OUTPUT);

  while (!Serial);
  Serial.println("LoRa Sender");
  LoRa.setPins(ss, rst, dio0);   
  LoRa.setSyncWord(0xA2);
  LoRa.setTxPower(20);
  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(62.5E3);
  while (!LoRa.begin(433920000))     
  {
    Serial.println(".");
    delay(500);
  }
  Serial.println("LoRa Initializing OK!");
}

void send_data(){
  int i;
  for(i=0;i<=(20);i++){
    LoRa.beginPacket();   
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
    
    delay(200);
  }
  Serial.println("");
}
 
void loop() 
{
  Serial.print("Sending packet: ");
  Serial.println(counter);

  if(digitalRead(hsen)==0){
    temp_count1=temp_count1+1;
    Serial.println("Water High...");
    Serial.println(temp_count1);
    if(temp_count1>=3){
      vstate1=0;
      vstate2=0;
      temp_count1=0;
      Serial.println("High....");
    }
  }else{
    temp_count1=0;
  }

  if(digitalRead(lsen)==0){
    temp_count2=temp_count2+1;
    Serial.println("Water Low...");
    Serial.println(temp_count2);
    if(temp_count2>=3){
      Serial.println("Low...");
      vstate1=1;
      vstate2=1;
      temp_count2=0;
    }
  }else{
    temp_count2=0;
  }


  if(digitalRead(lsen)!=0 && digitalRead(hsen)!=0){
    temp_count3=temp_count3+1;
    Serial.println("temp count");
    Serial.println(temp_count2);
    if(temp_count3>=10){
      Serial.println("Normal......");
      vstate1=2;
      vstate2=2;
      temp_count3=0;
    }
  }else{
    temp_count3=0;
  }

  counter++;
  if(counter>=200){
    counter=1;
  }
  send_data();
  digitalWrite(spin,LOW);
  delay(500);
  digitalWrite(spin,HIGH);
  delay(2000);
}
