// MicroSwap with <algorithm>: sorting and searching containers that do not
// fit in RAM, including objects with string members.
//
// Needs a core that ships the C++ standard library (ESP32, ESP8266, STM32,
// SAMD, RP2040, ...). On AVR the containers work, but <algorithm> is missing.

#include "SwapFile.h"
#include "Person.h"

#if VM_USE_STL
#include <algorithm>
#include <numeric>
#endif

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    beginSwapOrHalt();

#if VM_USE_STL
    VMVector<int> numbers;
    for (int i = 0; i < 3000; ++i) numbers.push_back((i * 7919) % 3000);

    std::sort(numbers.begin(), numbers.end());
    Serial.print(F("Sorted: "));
    Serial.println(std::is_sorted(numbers.begin(), numbers.end()) ? F("yes") : F("no"));
    Serial.print(F("Sum: "));
    Serial.println(std::accumulate(numbers.begin(), numbers.end(), 0L)); // 4498500

    VMVector<Person> people;
    people.emplace_back(30, "alice");
    people.emplace_back(22, "bob");
    people.emplace_back(27, "carol");
    people.emplace_back(35, "dave");

    std::sort(people.begin(), people.end(),
              [](const Person& a, const Person& b) { return a.age < b.age; });
    Serial.print(F("By age: "));
    printPeople(people);

    VMString text("the quick brown fox");
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return char(toupper(c)); });
    Serial.println(text.c_str());
#else
    Serial.println(F("<algorithm> is not available on this board."));
#endif
}

void loop() {}
