#pragma once

// Swap storage on a Microchip 23LC1024 SPI SRAM chip (128 KB).
//
// Unlike flash or SD cards, SRAM does not wear out, and it is much faster,
// which makes it a good place for swapped pages. Its contents are lost when
// power is removed, which does not matter for MicroSwap.

#include <Arduino.h>
#include <SPI.h>
#include <MicroSwap.h>

class SpiRamStorage : public VMStorage {
public:
    static const uint32_t kCapacity = 131072UL; // bytes

    explicit SpiRamStorage(uint8_t cs_pin) : _cs(cs_pin) {}

    void begin() {
        pinMode(_cs, OUTPUT);
        digitalWrite(_cs, HIGH);
        SPI.begin();

        // Sequential mode: reads and writes continue across the chip's pages.
        select();
        SPI.transfer(CMD_WRITE_MODE);
        SPI.transfer(MODE_SEQUENTIAL);
        deselect();
    }

    bool read(uint32_t offset, uint8_t* buf, size_t len) override {
        if (offset + len > kCapacity) return false;
        start(CMD_READ, offset);
        for (size_t i = 0; i < len; ++i) buf[i] = SPI.transfer(0);
        deselect();
        return true;
    }

    bool write(uint32_t offset, const uint8_t* buf, size_t len) override {
        if (offset + len > kCapacity) return false;
        start(CMD_WRITE, offset);
        for (size_t i = 0; i < len; ++i) SPI.transfer(buf[i]);
        deselect();
        return true;
    }

private:
    enum : uint8_t {
        CMD_READ        = 0x03,
        CMD_WRITE       = 0x02,
        CMD_WRITE_MODE  = 0x01,
        MODE_SEQUENTIAL = 0x40,
    };

    uint8_t _cs;

    void select() {
        SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
        digitalWrite(_cs, LOW);
    }

    void deselect() {
        digitalWrite(_cs, HIGH);
        SPI.endTransaction();
    }

    /// Send a command followed by a 24-bit address.
    void start(uint8_t command, uint32_t address) {
        select();
        SPI.transfer(command);
        SPI.transfer(uint8_t(address >> 16));
        SPI.transfer(uint8_t(address >> 8));
        SPI.transfer(uint8_t(address));
    }
};
