// The chip behind the simulated SPI bus: commands, addresses and the mode
// register as a 23LC1024 (24-bit addresses) or 23K256 (16-bit) handles them.
#pragma once

#include "SPI.h"

#include <vector>

struct FakeSram {
    uint8_t cs_pin = 10;
    bool present = true;
    size_t size = 131072;       ///< bytes the chip really has
    int address_bytes = 3;      ///< 3 for chips above 64 KB, else 2
    bool cs_low = false;
    uint8_t mode = 0x00;        ///< byte mode at power-up
    std::vector<uint8_t> cells;
    // The command being clocked in.
    int index = 0;
    uint8_t command = 0;
    uint32_t address = 0;
    // Faults the tests look for.
    int bytes_without_transaction = 0;
    int bytes_outside_select = 0;
    int bytes_not_sequential = 0;

    void reset(size_t chip_size, int addr_bytes) {
        size = chip_size;
        address_bytes = addr_bytes;
        cells.assign(size, 0xEE);
        mode = 0x00;
        present = true;
        cs_low = false;
        bytes_without_transaction = bytes_outside_select = bytes_not_sequential = 0;
    }
};

extern FakeSram g_sram;
