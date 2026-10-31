// VMArray: fixed-size arrays in virtual memory, including global ones.
//
// Wiring: SD card on the board's default SPI pins (see SwapFile.h).

#include "SwapFile.h"

struct Point {
    int x;
    int y;
};

// Arrays can be globals: memory is allocated on first access, after begin().
VMArray<int, 32> readings;
VMArray<Point, 8> path;

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    beginSwapOrHalt();

    // Elements start value-initialized: 0 for numbers and plain structs.
    Serial.print(F("readings[5] before writing: "));
    Serial.println(readings[5]);

    for (size_t i = 0; i < readings.size(); ++i) readings[i] = int(i * i);
    Serial.print(F("readings.at(10) = "));
    Serial.println(readings.at(10)); // 100

    // Iterators work like std::array's.
    long sum = 0;
    for (VMArray<int, 32>::const_iterator it = readings.begin(); it != readings.end(); ++it) sum += *it;
    Serial.print(F("Sum of squares 0..31: "));
    Serial.println(sum); // 10416

    Serial.print(F("Last three, reversed: "));
    VMArray<int, 32>::reverse_iterator rit = readings.rbegin();
    for (int n = 0; n < 3; ++n, ++rit) {
        Serial.print(*rit);
        Serial.print(' ');
    }
    Serial.println();

    // Arrays of structs.
    for (size_t i = 0; i < path.size(); ++i) {
        path[i].x = int(i);
        path[i].y = int(i) * 2;
    }
    Serial.print(F("path.back() = ("));
    Serial.print(path.back().x);
    Serial.print(F(", "));
    Serial.print(path.back().y);
    Serial.println(F(")"));

    // Copies are independent.
    VMArray<int, 32> snapshot(readings);
    readings.fill(-1);
    Serial.print(F("After fill(-1): readings[3]="));
    Serial.print(readings[3]);
    Serial.print(F(", snapshot[3]="));
    Serial.println(snapshot[3]); // 9

    readings.clear(); // every element back to 0
    Serial.print(F("After clear(): readings[3]="));
    Serial.println(readings[3]);
}

void loop() {}
