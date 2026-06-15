#include "zmpt101b.h"

ZMPT101B::ZMPT101B(int analogPin, float calFactor) {
    pin = analogPin;
    calibration = calFactor;
}

float ZMPT101B::readVoltageRMS(int samples) {
    float sum = 0;

    for (int i = 0; i < samples; i++) {
        int adc = analogRead(pin);

        // ESP8266 ADC scaling (0–1V)
        float voltage = (adc / 1023.0) * 1.0;

        sum += voltage * voltage;
        delayMicroseconds(200);
    }

    float mean = sum / samples;
    float rms = sqrt(mean);

    return rms * calibration;
}