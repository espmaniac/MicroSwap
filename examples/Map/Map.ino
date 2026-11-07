// MicroSwap map: counting the words of a text by key, and a table looked up
// by name, like std::map but in virtual memory.
//
// Wiring: SD card on the board's default SPI pins, chip select on SD_CS_PIN
// (see SwapFile.h).

#include "SwapFile.h"

void onVMError(VMError error) {
    Serial.print(F("MicroSwap error: "));
    Serial.println(vm_error_name(error));
    while (true) delay(1000);
}

// In flash (PROGMEM): on AVR a plain string would take RAM.
const char text[] PROGMEM =
    "the quick brown fox jumps over the lazy dog the dog sleeps "
    "and the fox runs over the hill while the dog dreams";

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    VMManager::instance().set_error_handler(onVMError);
    beginSwapOrHalt();

    // ---- Counting words: operator[] inserts 0 the first time ----
    VMMap<VMString, int> counts;
    char word[16];
    size_t length = 0;
    for (size_t i = 0;; ++i) {
        const char c = char(pgm_read_byte(&text[i]));
        if (c == ' ' || c == '\0') {
            if (length > 0) {
                word[length] = '\0';
                counts[word]++;
                length = 0;
            }
            if (c == '\0') break;
        } else if (length + 1 < sizeof(word)) {
            word[length++] = c;
        }
    }
    Serial.print(F("Different words: "));
    Serial.println(counts.size());
    // In key order, as with std::map.
    for (const VMPair<VMString, int>& entry : counts) {
        Serial.print(entry.first.c_str());
        Serial.print(F(" "));
        Serial.println(entry.second);
    }
    // Looking up by a C string allocates nothing.
    Serial.print(F("\"the\" appears "));
    Serial.print(counts.at("the"));
    Serial.println(F(" times"));
    Serial.print(F("Has \"cat\": "));
    Serial.println(counts.contains("cat") ? F("yes") : F("no"));

    // ---- A table by number: insert, update, erase ----
    VMMap<int, VMString> sensors;
    sensors.insert(12, "kitchen");
    sensors.insert(3, "garden");
    sensors.insert(7, "garage");
    sensors.insert_or_assign(7, "workshop");
    sensors.erase(3);
    for (const VMPair<int, VMString>& entry : sensors) {
        Serial.print(F("sensor "));
        Serial.print(entry.first);
        Serial.print(F(": "));
        Serial.println(entry.second.c_str());
    }
}

void loop() {}
