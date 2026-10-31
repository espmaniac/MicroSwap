// MicroSwap basic example: a vector bigger than the RAM it uses.
//
// Wiring: SD card on the board's default SPI pins, chip select on SD_CS_PIN
// (see SwapFile.h).

#include "SwapFile.h"

void onVMError(VMError error) {
    Serial.print(F("MicroSwap error: "));
    Serial.println(vm_error_name(error));
    while (true) delay(1000);
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    VMManager& vm = VMManager::instance();
    vm.set_error_handler(onVMError);
    beginSwapOrHalt();

    Serial.print(F("RAM used for pages: "));
    Serial.print(vm.get_page_size() * vm.get_resident_pages());
    Serial.print(F(" bytes, virtual memory: "));
    Serial.print(vm.get_page_size() * vm.get_page_count());
    Serial.println(F(" bytes"));

    // 4000 bytes of data, much more than an Uno's 2 KB of RAM.
    VMVector<long> squares;
    for (long i = 0; i < 1000; ++i) squares.push_back(i * i);

    long long sum = 0;
    for (size_t i = 0; i < squares.size(); ++i) sum += squares[i];
    Serial.print(F("Sum of squares 0..999: "));
    Serial.println((unsigned long)sum); // 332833500

    VMString greeting("Hello");
    greeting += ", MicroSwap!";
    Serial.println(greeting.c_str());

    Serial.print(F("Pages swapped in: "));
    Serial.print(vm.swap_in_count());
    Serial.print(F(", out: "));
    Serial.println(vm.swap_out_count());
}

void loop() {}
