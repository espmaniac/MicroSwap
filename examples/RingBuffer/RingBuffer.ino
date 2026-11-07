// MicroSwap ring buffer: the last 2000 readings of a sensor, in about 0.7 KB
// of RAM on an Uno. When it is full, each new reading replaces the oldest.
//
// Wiring: SD card on the board's default SPI pins, chip select on SD_CS_PIN
// (see SwapFile.h).

#include "SwapFile.h"

void onVMError(VMError error) {
    Serial.print(F("MicroSwap error: "));
    Serial.println(vm_error_name(error));
    while (true) delay(1000);
}

VMRingBuffer<int> readings(2000);

// A stand-in for analogRead(A0): a slow wave, so the numbers are predictable.
int readSensor(long t) {
    return 500 + int((t % 200) - 100);
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    VMManager::instance().set_error_handler(onVMError);
    beginSwapOrHalt();

    // 5000 readings: the buffer keeps the last 2000.
    for (long t = 0; t < 5000; ++t) readings.push_back(readSensor(t));

    long sum = 0;
    int lowest = readings.front();
    int highest = readings.front();
    for (int value : readings) {
        sum += value;
        if (value < lowest) lowest = value;
        if (value > highest) highest = value;
    }
    Serial.print(F("Readings kept: "));
    Serial.print(readings.size());
    Serial.print(F(" of the last "));
    Serial.println(readings.capacity());
    Serial.print(F("Oldest: "));
    Serial.print(readings.front());
    Serial.print(F(", newest: "));
    Serial.println(readings.back());
    Serial.print(F("Average: "));
    Serial.print(sum / long(readings.size()));
    Serial.print(F(", min: "));
    Serial.print(lowest);
    Serial.print(F(", max: "));
    Serial.println(highest);

    // As a queue: take the oldest out; try_push_back refuses when full.
    for (int i = 0; i < 3; ++i) readings.pop_front();
    Serial.print(F("After taking 3 out: "));
    Serial.print(readings.size());
    Serial.print(F(", room for one more: "));
    Serial.println(readings.try_push_back(1) ? F("yes") : F("no"));
}

void loop() {}
