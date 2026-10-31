// VMPin: keeping a reference valid while other pages come and go.
//
// Wiring: SD card on the board's default SPI pins (see SwapFile.h).

// Pins must leave 4 resident pages unpinned for the containers' own work,
// so AVR's default of 4 allows none. Five smaller pages allow one and use
// less RAM than the default (5 * 64 bytes). Must come before the include.
#if defined(__AVR__)
#define VM_RESIDENT_PAGES 5
#define VM_PAGE_SIZE      64
#endif

#include "SwapFile.h"

// Print errors and continue, so the TooManyPins case below can be shown.
void onVMError(VMError error) {
    Serial.print(F("  (MicroSwap error: "));
    Serial.print(vm_error_name(error));
    Serial.println(F(")"));
}

/// Read every element, which evicts every page that is not pinned.
long sumAll(const VMVector<long>& v) {
    long sum = 0;
    for (size_t i = 0; i < v.size(); ++i) sum += v[i];
    return sum;
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    VMManager::instance().set_error_handler(onVMError);
    beginSwapOrHalt();

    // Six pages of data: more than fit in RAM on any board.
    const long count = long(VMManager::instance().get_page_size() / sizeof(long)) * 6;
    VMVector<long> values;
    for (long i = 0; i < count; ++i) values.push_back(i);

    // ---- Keep a reference to one element ----
    {
        VMPin<long> first(values[0]);      // or: auto first = vm_pin(values[0]);
        long total = sumAll(values);       // other pages are swapped in and out
        *first = total;                    // still the same element
    }                                      // unpinned here
    Serial.print(F("values[0] = sum of 0.."));
    Serial.print(count - 1);
    Serial.print(F(" = "));
    Serial.println(values[0]);

    // ---- Keep a C string while doing other work ----
    VMString message("A message that is printed after a lot of other work");
    {
        VMPin<const char> text(message.c_str());
        sumAll(values);
        Serial.println(text.get());
    }

    // ---- Pins are limited ----
    // At most VM_RESIDENT_PAGES - 4 different pages can be pinned at once.
    VMPin<long> a(values[0]);
    VMPin<long> b(values[count - 1]); // the last page
    Serial.print(F("First pin valid: "));
    Serial.println(a.valid() ? F("yes") : F("no"));
    Serial.print(F("Second pin valid: "));
    Serial.println(b.valid() ? F("yes") : F("no (raise VM_RESIDENT_PAGES)"));
}

void loop() {}
