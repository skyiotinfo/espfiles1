#ifndef PZEM_READER_H
#define PZEM_READER_H

#include <Arduino.h>
#include <PZEM004Tv30.h>

// Structure to hold all readings
struct PZEMData {
    float voltage;
    float current;
    float power;
    float energy;
    float frequency;
    float pf;
    float apparentPower;
    float reactivePower;
};

// Class for PZEM
class PZEMReader {
  private:
    PZEM004Tv30* pzem;

  public:
    PZEMReader(Stream& serial);
    PZEMData readData();
    float zeroIfNan(float v);
};

#endif