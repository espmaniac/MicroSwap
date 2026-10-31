#pragma once

// In its own header so the Arduino IDE sees the type before the function
// prototypes it generates for the sketch.

#include <Arduino.h>
#include <MicroSwap.h>

struct Person {
    int age;
    VMString name;

    Person() : age(0) {}
    Person(int a, const char* n) : age(a), name(n) {}
};

inline void printPeople(const VMVector<Person>& people) {
    for (size_t i = 0; i < people.size(); ++i) {
        Serial.print(people[i].name.c_str());
        Serial.print(F(" ("));
        Serial.print(people[i].age);
        Serial.print(i + 1 < people.size() ? F("), ") : F(")\n"));
    }
}
