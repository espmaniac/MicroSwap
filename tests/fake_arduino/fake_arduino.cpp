#include "fake_sram.h"

SPIClass SPI;
FakeSram g_sram;

void pinMode(uint8_t, uint8_t) {}

void digitalWrite(uint8_t pin, uint8_t level) {
    if (pin != g_sram.cs_pin) return;
    g_sram.cs_low = level == LOW;
    if (g_sram.cs_low) g_sram.index = 0; // a new command starts
}

uint8_t SPIClass::transfer(uint8_t out) {
    FakeSram& c = g_sram;
    if (!in_transaction) c.bytes_without_transaction++;
    if (!c.cs_low) {
        c.bytes_outside_select++;
        return 0xFF;
    }
    if (!c.present) return 0xFF; // nothing drives MISO; the pull-up reads ones
    const int n = c.index++;
    if (n == 0) {
        c.command = out;
        c.address = 0;
        return 0xFF;
    }
    if (c.command == 0x01) { // write mode register
        if (n == 1) c.mode = out;
        return 0xFF;
    }
    if (c.command == 0x05) return c.mode; // read mode register
    if (c.command != 0x02 && c.command != 0x03) return 0xFF;
    if (n <= c.address_bytes) {
        c.address = (c.address << 8) | out;
        return 0xFF;
    }
    // Data: in sequential mode the address runs on and wraps at the chip's end;
    // a smaller chip ignores the address bits it does not have.
    if (c.mode != 0x40 && n > c.address_bytes + 1) c.bytes_not_sequential++;
    const uint32_t at = uint32_t((c.address + uint32_t(n - c.address_bytes - 1)) % c.size);
    if (c.command == 0x02) {
        c.cells[at] = out;
        return 0xFF;
    }
    return c.cells[at];
}
