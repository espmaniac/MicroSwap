// VMVector: every operation, on a vector bigger than the RAM it uses.
//
// Wiring: SD card on the board's default SPI pins (see SwapFile.h).

#include "SwapFile.h"

void printVector(const __FlashStringHelper* label, const VMVector<int>& v) {
    Serial.print(label);
    Serial.print(F(" (size "));
    Serial.print(v.size());
    Serial.print(F("): "));
    for (VMVector<int>::const_iterator it = v.begin(); it != v.end(); ++it) {
        Serial.print(*it);
        Serial.print(' ');
    }
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    beginSwapOrHalt();

    // ---- Adding and removing elements ----
    VMVector<int> v;
    for (int i = 1; i <= 5; ++i) v.push_back(i * 10);  // 10 20 30 40 50
    v.emplace_back(60);                                // construct in place
    v.insert(v.begin() + 1, 15);                       // 10 15 20 30 40 50 60
    v.erase(v.begin() + 3);                            // 10 15 20 40 50 60
    v.pop_back();                                      // 10 15 20 40 50
    printVector(F("After edits"), v);

    // ---- Element access ----
    Serial.print(F("front="));
    Serial.print(v.front());
    Serial.print(F(" back="));
    Serial.print(v.back());
    Serial.print(F(" v[2]="));
    Serial.print(v[2]);
    Serial.print(F(" at(1)="));
    Serial.println(v.at(1));

    v[0] = 5; // write through operator[]

    // ---- Iterators, forward and reverse ----
    Serial.print(F("Reversed: "));
    for (VMVector<int>::reverse_iterator it = v.rbegin(); it != v.rend(); ++it) {
        Serial.print(*it);
        Serial.print(' ');
    }
    Serial.println();

    // ---- Flat and paged storage ----
    // Small vectors live in one block and have data(); large ones span pages.
    Serial.print(F("Small vector is flat: "));
    Serial.println(v.is_flat() ? F("yes") : F("no"));
    int* raw = v.data();
    Serial.print(F("data()[1] = "));
    Serial.println(raw[1]);

    VMVector<int> big;
    big.reserve(2000);
    for (int i = 0; i < 2000; ++i) big.push_back(i);   // 4000+ bytes
    Serial.print(F("Big vector is flat: "));
    Serial.print(big.is_flat() ? F("yes") : F("no"));
    Serial.print(F(", capacity "));
    Serial.println(big.capacity());

    long sum = 0;
    for (size_t i = 0; i < big.size(); ++i) sum += big[i];
    Serial.print(F("Sum 0..1999 = "));
    Serial.println(sum); // 1999000

    // ---- Size changes ----
    big.resize(10);          // drops elements, frees pages
    big.shrink_to_fit();
    big.resize(12, -1);      // appends two -1
    printVector(F("Resized"), big);

    // ---- Copy, assign, swap, compare ----
    VMVector<int> copy(v);
    copy[0] = 999;
    Serial.print(F("Copy is independent: v[0]="));
    Serial.print(v[0]);
    Serial.print(F(" copy[0]="));
    Serial.println(copy[0]);

    VMVector<int> filled(3, 7);       // 7 7 7
    filled.assign(4, 1);              // 1 1 1 1
    filled.swap(copy);
    printVector(F("Swapped"), filled);
    Serial.print(F("filled == copy: "));
    Serial.println(filled == copy ? F("yes") : F("no"));
    Serial.print(F("copy < filled: "));
    Serial.println(copy < filled ? F("yes") : F("no"));

    // ---- Vectors of vectors ----
    // A container stored inside another one works like any other.
    VMVector<VMVector<int> > rows;
    for (int r = 0; r < 3; ++r) {
        rows.emplace_back();
        for (int c = 0; c <= r; ++c) rows[r].push_back(r * 10 + c);
    }
    printVector(F("rows[2]"), rows[2]); // 20 21 22

    big.clear(); // frees all of its storage
    Serial.print(F("Free pages: "));
    Serial.println(VMManager::instance().free_pages());
}

void loop() {}
