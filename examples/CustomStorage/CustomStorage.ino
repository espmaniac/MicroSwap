// Swapping to an external SPI SRAM chip instead of an SD card, by
// implementing VMStorage (see SpiRamStorage.h).
//
// Wiring: 23LC1024 on the board's default SPI pins, chip select on SRAM_CS_PIN.

// The chip holds 128 KB, so the virtual memory must not be larger:
// VM_PAGE_SIZE * VM_PAGE_COUNT <= 131072. AVR's defaults (128 * 64) fit;
// on other boards use 128 pages of 1 KB. Must come before the include.
#if !defined(__AVR__)
#define VM_PAGE_SIZE  1024
#define VM_PAGE_COUNT 128
#endif

#include "SpiRamStorage.h"

static_assert(uint32_t(VM_PAGE_SIZE) * VM_PAGE_COUNT <= SpiRamStorage::kCapacity,
              "virtual memory is larger than the SRAM chip");

const uint8_t SRAM_CS_PIN = SS; // change to your chip's chip select pin

SpiRamStorage sram(SRAM_CS_PIN);

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    sram.begin();
    VMManager& vm = VMManager::instance();
    vm.begin(sram);

    VMVector<long> values;
    const long count = long(vm.get_page_size() / sizeof(long)) * 10; // 10 pages
    for (long i = 0; i < count; ++i) values.push_back(i * 3);

    long wrong = 0;
    for (long i = 0; i < count; ++i)
        if (values[i] != i * 3) wrong++;

    Serial.print(F("Checked "));
    Serial.print(count);
    Serial.print(F(" values through the SRAM, wrong: "));
    Serial.println(wrong); // 0 if the chip is wired correctly

    Serial.print(F("Pages swapped in: "));
    Serial.print(vm.swap_in_count());
    Serial.print(F(", out: "));
    Serial.println(vm.swap_out_count());
}

void loop() {}
