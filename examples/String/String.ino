// VMString: building, searching and editing strings in virtual memory.
//
// Wiring: SD card on the board's default SPI pins (see SwapFile.h).

#include "SwapFile.h"

void show(const __FlashStringHelper* label, const VMString& s) {
    Serial.print(label);
    Serial.print(F(": \""));
    Serial.print(s.c_str());
    Serial.print(F("\" (length "));
    Serial.print(s.size());
    Serial.println(F(")"));
}

void printPosition(const __FlashStringHelper* label, size_t pos) {
    Serial.print(label);
    if (pos == VMString::npos) Serial.println(F("not found"));
    else Serial.println(pos);
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    beginSwapOrHalt();

    // ---- Building ----
    VMString s("temperature");
    s += '=';
    s.append("21.5");
    s.append(1, 'C');
    show(F("Built"), s);

    VMString line = VMString("[") + s + "]";
    show(F("Concatenated"), line);

    // ---- Searching ----
    printPosition(F("find(\"=\")        "), s.find('='));
    printPosition(F("find(\"21\")       "), s.find("21"));
    printPosition(F("rfind('e')        "), s.rfind('e'));
    printPosition(F("find_first_of(\"0123456789\") "), s.find_first_of("0123456789"));
    printPosition(F("find(\"humidity\") "), s.find("humidity"));

    // ---- Extracting ----
    size_t eq = s.find('=');
    VMString key = s.substr(0, eq);
    VMString value = s.substr(eq + 1);
    show(F("Key"), key);
    show(F("Value"), value);

    char buffer[8];
    size_t n = value.copy(buffer, sizeof(buffer) - 1);
    buffer[n] = '\0';
    Serial.print(F("Copied to a char buffer: "));
    Serial.println(buffer);

    // ---- Editing ----
    s.replace(0, eq, "temp");   // "temp=21.5C"
    s.insert(0, "> ");          // "> temp=21.5C"
    s.erase(s.size() - 1);      // "> temp=21.5"
    s.push_back('!');
    show(F("Edited"), s);

    // Character access and iteration.
    s[0] = '#';
    int digits = 0;
    for (VMString::const_iterator it = s.begin(); it != s.end(); ++it)
        if (*it >= '0' && *it <= '9') digits++;
    Serial.print(F("Digits: "));
    Serial.println(digits); // 3

    // ---- Comparing ----
    VMString a("apple");
    VMString b("banana");
    Serial.print(F("apple < banana: "));
    Serial.println(a < b ? F("yes") : F("no"));
    Serial.print(F("a == \"apple\": "));
    Serial.println(a == "apple" ? F("yes") : F("no"));
    Serial.print(F("a.compare(b): "));
    Serial.println(a.compare(b));

    // ---- Capacity ----
    // A string is stored in one page, so its length is limited.
    Serial.print(F("max_size(): "));
    Serial.println(VMString::max_size());

    VMString empty;
    Serial.print(F("Empty string is empty(): "));
    Serial.println(empty.empty() ? F("yes") : F("no"));
}

void loop() {}
