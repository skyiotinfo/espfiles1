#include <Servo.h>
Servo mg995;   // Create servo object
void setup() {
mg995.attach(D4, 500, 2500);
}
void loop() {
  // Move servo to 180 degrees
  mg995.write(180);
  delay(2000);   // Wait for 2 seconds
  // Move servo back to 0 degrees
  mg995.write(0);
  delay(2000);   // Wait for 2 seconds

    // Wait for 2 seconds
}