#include "fake_memory.h"

SPIClass SPI;
FakeMemory g_chip;

void pinMode(uint8_t, uint8_t) {}
void delayMicroseconds(unsigned int) {}

void digitalWrite(uint8_t pin, uint8_t level) {
    FakeMemory& c = g_chip;
    if (pin != c.cs_pin) return;
    const bool low = level == LOW;
    if (c.cs_low && !low) {
        // End of a command: a write clears the write enable latch, as on FRAM.
        if (c.command == 0x02 && c.index > 1) c.wel = false;
        if (c.command == 0x06 && c.index == 1) c.wel = true;
        if (c.command == 0x99 && c.index == 1) c.resets++;
    }
    c.cs_low = low;
    if (low) {
        c.index = 0;
        c.data_in_select = 0;
    }
}

uint8_t SPIClass::transfer(uint8_t out) {
    FakeMemory& c = g_chip;
    if (!in_transaction) c.bytes_without_transaction++;
    if (!c.cs_low) {
        c.bytes_outside_select++;
        return 0xFF;
    }
    if (!c.present) return 0xFF; // nothing drives MISO; the pull-up reads ones
    if (clock > c.max_clock && c.index == 0) c.clock_too_fast++;
    const int n = c.index++;
    if (n == 0) {
        c.command = out;
        c.address = 0;
        return 0xFF;
    }
    if (c.mode_register && c.command == 0x01) { // write mode register
        if (n == 1) c.mode = out;
        return 0xFF;
    }
    if (c.mode_register && c.command == 0x05) return c.mode;
    if (c.command != 0x02 && c.command != c.read_cmd) return 0xFF;
    if (n <= c.address_bytes) {
        c.address = (c.address << 8) | out;
        return 0xFF;
    }
    const int dummy = c.command == c.read_cmd ? c.read_dummy : 0;
    if (n <= c.address_bytes + dummy) return 0xFF;
    const size_t k = size_t(n - c.address_bytes - dummy - 1); // data byte index in this select
    if (c.mode_register && c.mode != 0x40 && k > 0) c.bytes_not_sequential++;
    if (c.max_data_per_select && ++c.data_in_select > c.max_data_per_select) {
        c.selects_too_long++;
        return 0x00; // the chip lost it
    }
    uint32_t at;
    if (c.page_bytes) {
        const uint32_t base = c.address - c.address % uint32_t(c.page_bytes);
        const uint32_t in_page = uint32_t((c.address % c.page_bytes + k) % c.page_bytes);
        if ((c.address % c.page_bytes) + k >= c.page_bytes) c.page_crossings++;
        at = uint32_t((base + in_page) % c.size);
    } else {
        at = uint32_t((c.address + k) % c.size);
    }
    if (c.command == 0x02) {
        if (c.write_enable && !c.wel) {
            c.writes_without_enable++;
            return 0xFF;
        }
        c.cells[at] = out;
        return 0xFF;
    }
    return c.cells[at];
}
