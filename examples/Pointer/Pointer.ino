// VMPtr and make_vm: single objects in virtual memory.
//
// Wiring: SD card on the board's default SPI pins (see SwapFile.h).

#include "SwapFile.h"

struct Sensor {
    VMString name;
    float last_value;
    unsigned long samples;

    Sensor(const char* n) : name(n), last_value(0), samples(0) {}

    void record(float value) {
        last_value = value;
        samples++;
    }
};

void printSensor(const VMPtr<Sensor>& sensor) {
    Serial.print(sensor->name.c_str());
    Serial.print(F(": "));
    Serial.print(sensor->last_value);
    Serial.print(F(" ("));
    Serial.print(sensor->samples);
    Serial.println(F(" samples)"));
}

VMPtr<Sensor> outdoor; // null until make_vm

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    beginSwapOrHalt();

    // Construct the object in virtual memory, like std::make_unique.
    outdoor = make_vm<Sensor>("outdoor");
    if (!outdoor) {
        Serial.println(F("Out of virtual memory"));
        return;
    }

    outdoor->record(12.5);
    outdoor->record(13.0);
    printSensor(outdoor);

    // Copies point to the same object, like raw pointers.
    VMPtr<Sensor> alias = outdoor;
    alias->record(14.25);
    Serial.print(F("Same object through the copy: "));
    printSensor(outdoor);

    // Small values work too.
    VMPtr<long> counter = make_vm<long>(100);
    *counter += 1;
    Serial.print(F("*counter = "));
    Serial.println(*counter); // 101

    // Objects live until destroy(); then the pointer is null.
    counter.destroy();
    Serial.print(F("counter after destroy(): "));
    Serial.println(counter ? F("valid") : F("null"));
}

void loop() {
    // The object stays in virtual memory between loop() calls.
    if (!outdoor) return;
    outdoor->record(analogRead(A0) * 0.1f);
    printSensor(outdoor);
    delay(2000);
}
