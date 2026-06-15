#ifndef ZMPT101B_H
#define ZMPT101B_H

#include <Arduino.h>

class ZMPT101B {
  private:
    int pin;
    float calibration;

  public:
    ZMPT101B(int analogPin, float calFactor = 1.0);

    float readVoltageRMS(int samples = 500);
};

#endif