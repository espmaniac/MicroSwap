// Swapping to an SPI SRAM chip (23LC1024 and alike) with the storage that
// comes with MicroSwap: no SD card and no file system, faster, and SRAM does
// not wear out.
//
// Wiring: the chip on the board's default SPI pins (SCK, MOSI, MISO), chip
// select on SRAM_CS_PIN, VCC 2.5-5.5 V (23LC1024; 23K256 takes 2.7-3.6 V).

// The chip holds 128 KB, so the virtual memory must not be larger:
// VM_PAGE_SIZE * VM_PAGE_COUNT <= 131072. AVR's defaults (128 * 64) fit;
// on other boards use 128 pages of 1 KB. Must come before the include.
#if !defined(__AVR__)
#define VM_PAGE_SIZE  1024
#define VM_PAGE_COUNT 128
#endif

#include <MicroSwapSpiSram.h>

const uint8_t SRAM_CS_PIN = SS; // change to your chip's chip select pin

// For a 23LC512 pass 65536, for a 23K256 32768, as the second argument.
VMSpiSramStorage sram(SRAM_CS_PIN);

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    if (!sram.begin()) {
        Serial.println(F("No SRAM chip answers on SRAM_CS_PIN, or it is smaller than declared"));
        while (true) delay(1000);
    }
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
    Serial.println(wrong); // 0

    Serial.print(F("Pages swapped in: "));
    Serial.print(vm.swap_in_count());
    Serial.print(F(", out: "));
    Serial.println(vm.swap_out_count());
}

void loop() {}
