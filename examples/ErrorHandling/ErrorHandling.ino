// Handling MicroSwap errors without exceptions.
//
// The default error handler calls abort(). This sketch installs one that
// logs and returns, so each error can be seen; failed operations then do
// nothing.
//
// Wiring: SD card on the board's default SPI pins (see SwapFile.h).

#include "SwapFile.h"

void onVMError(VMError error) {
    Serial.print(F("  -> error handler: "));
    Serial.println(vm_error_name(error));
}

void report(const __FlashStringHelper* what) {
    VMManager& vm = VMManager::instance();
    Serial.print(what);
    Serial.print(F(": last_error() = "));
    Serial.println(vm_error_name(vm.last_error()));
    vm.clear_error();
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    VMManager& vm = VMManager::instance();
    vm.set_error_handler(onVMError);
    beginSwapOrHalt();

    // ---- Index out of range ----
    VMVector<int> v;
    v.push_back(1);
    v.at(5); // returns a placeholder instead of throwing
    report(F("v.at(5)"));

    VMVector<int> empty;
    empty.pop_back();
    report(F("pop_back() on an empty vector"));

    // ---- Object too large ----
    VMString s("short");
    s.resize(VMString::max_size() + 1, 'x');
    Serial.print(F("String unchanged after a failed resize: \""));
    Serial.print(s.c_str());
    Serial.println(F("\""));
    report(F("resize() past max_size()"));

    // ---- Null pointer ----
    VMPtr<long> nothing;
    *nothing = 1; // writes to a placeholder
    report(F("dereferencing a null VMPtr"));

    // ---- Out of virtual memory ----
    Serial.print(F("Free pages before: "));
    Serial.println(vm.free_pages());

    VMVector<long> hog;
    while (vm.last_error() == VMError::None) hog.push_back(0); // until a push fails
    Serial.print(F("Stored "));
    Serial.print(hog.size());
    Serial.print(F(" longs, free pages: "));
    Serial.println(vm.free_pages());
    report(F("filling all virtual memory"));

    hog.clear();
    Serial.print(F("Free pages after clear(): "));
    Serial.println(vm.free_pages());

    // ---- Back to the default handler ----
    // From here on an error stops the sketch with abort().
    vm.set_error_handler(nullptr);
}

void loop() {}
