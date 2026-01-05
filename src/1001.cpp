#include <Arduino.h>

const int buttonPin = A0;      // Button connected to A0
int buttonState = 0;            // Current reading
int lastButtonState = 1023;     // Previous reading (initialize high)

void setup() {
  Serial.begin(115200);
}

void loop() {
  buttonState = analogRead(buttonPin); // Read A0

  // Check for transition from unpressed to pressed
  if (buttonState <= 10 && lastButtonState > 10) { // pressed when <=10
    Serial.println("Hello");
  }

  lastButtonState = buttonState; // Save state for next loop
  delay(50);                     // small debounce
}
