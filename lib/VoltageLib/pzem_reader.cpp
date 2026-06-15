#include "pzem_reader.h"

PZEMReader::PZEMReader(Stream& serial) {
    pzem = new PZEM004Tv30(serial);
}

float PZEMReader::zeroIfNan(float v) {
    if (isnan(v)) return 0;
    return v;
}

PZEMData PZEMReader::readData() {
    PZEMData data;

    data.voltage = zeroIfNan(pzem->voltage());
    data.current = zeroIfNan(pzem->current());
    data.power   = zeroIfNan(pzem->power());
    data.energy  = zeroIfNan(pzem->energy());
    data.frequency = zeroIfNan(pzem->frequency());
    data.pf = zeroIfNan(pzem->pf());

    // Apparent Power (VA)
    if (data.pf == 0)
        data.apparentPower = 0;
    else
        data.apparentPower = data.power / data.pf;

    // Reactive Power (VAR)
    if (data.pf == 0)
        data.reactivePower = 0;
    else
        data.reactivePower = data.apparentPower * sqrt(1 - sq(data.pf));

    return data;
}