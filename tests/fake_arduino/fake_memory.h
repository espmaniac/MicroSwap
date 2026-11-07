// The chip behind the simulated SPI bus. It behaves as the chip families of
// MicroSwapSpiMemory.h do, and counts what a real chip would not accept.
#pragma once

#include "SPI.h"

#include <vector>

struct FakeMemory {
    uint8_t cs_pin = 10;
    bool present = true;
    size_t size = 131072;          ///< bytes the chip really has; higher addresses wrap (mirror)
    int address_bytes = 3;
    bool mode_register = false;    ///< 23LC: byte mode until sequential mode is set
    bool write_enable = false;     ///< FRAM: writes ignored unless write enable came first
    size_t page_bytes = 0;         ///< PSRAM: a burst wraps inside its page
    size_t max_data_per_select = 0; ///< PSRAM refresh: more data in one select is lost
    uint8_t read_cmd = 0x03;       ///< its read command (0x0B: fast read)
    uint8_t read_dummy = 0;        ///< bytes between address and data of a read
    uint32_t max_clock = 20000000;

    std::vector<uint8_t> cells;
    bool cs_low = false;
    uint8_t mode = 0x00;
    bool wel = false;
    int resets = 0;
    // The command being clocked in.
    int index = 0;
    uint8_t command = 0;
    uint32_t address = 0;
    size_t data_in_select = 0;
    // Faults.
    int bytes_without_transaction = 0;
    int bytes_outside_select = 0;
    int bytes_not_sequential = 0;
    int writes_without_enable = 0;
    int page_crossings = 0;
    int selects_too_long = 0;
    int clock_too_fast = 0;

    void reset(size_t chip_size, int addr_bytes) {
        *this = FakeMemory();
        size = chip_size;
        address_bytes = addr_bytes;
        cells.assign(size, 0xEE);
    }

    int faults() const {
        return bytes_without_transaction + bytes_outside_select + bytes_not_sequential + writes_without_enable +
               page_crossings + selects_too_long + clock_too_fast;
    }
};

extern FakeMemory g_chip;
