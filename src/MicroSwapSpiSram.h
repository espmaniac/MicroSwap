#pragma once

/**
 * @file MicroSwapSpiSram.h
 * @brief Swap storage on a Microchip SPI SRAM chip: 23LC1024 / 23A1024
 *        (128 KB), 23LC512 / 23A512 (64 KB), 23K256 (32 KB), 23K640 (8 KB).
 *
 * SRAM does not wear out, needs no file system and is faster than an SD
 * card, which makes it a good place for swapped pages. Its contents are lost
 * when power is removed, which does not matter for swapping.
 *
 *     #include <MicroSwapSpiSram.h>
 *     VMSpiSramStorage sram(10);              // chip select on pin 10
 *     void setup() {
 *         if (!sram.begin()) { ... }          // no chip, or too small
 *         VMManager::instance().begin(sram);
 *     }
 *
 * The virtual memory (VM_PAGE_SIZE * VM_PAGE_COUNT) must fit in the chip.
 */

#include <Arduino.h>
#include <SPI.h>

#include "MicroSwap.h"

class VMSpiSramStorage : public VMStorage {
public:
    /**
     * @param cs_pin    chip select pin.
     * @param capacity  bytes on the chip: 131072 (23LC1024), 65536 (23LC512),
     *                  32768 (23K256), 8192 (23K640). Chips above 64 KB take
     *                  24-bit addresses, the others 16-bit ones.
     * @param clock_hz  SPI clock; the chips take up to 16 or 20 MHz.
     */
    explicit VMSpiSramStorage(uint8_t cs_pin, uint32_t capacity = 131072UL, uint32_t clock_hz = 8000000UL,
                              SPIClass& spi = SPI)
        : _spi(spi), _capacity(capacity), _clock_hz(clock_hz), _cs(cs_pin) {}

    /**
     * @brief Set the chip up and check it.
     * @return False if no chip answers, if it holds less than `capacity`
     *         (the second half would mirror the first), or if the virtual
     *         memory does not fit in `capacity`.
     */
    bool begin() {
        pinMode(_cs, OUTPUT);
        digitalWrite(_cs, HIGH);
        _spi.begin();
        if (uint32_t(VM_PAGE_SIZE) * VM_PAGE_COUNT > _capacity) return false;

        // Sequential mode: reads and writes run on across the chip's own pages.
        select();
        _spi.transfer(kWriteMode);
        _spi.transfer(kSequential);
        deselect();
        select();
        _spi.transfer(kReadMode);
        const uint8_t mode = _spi.transfer(0xFF);
        deselect();
        // A missing chip reads as all ones or all zeros.
        if ((mode & kModeMask) != kSequential) return false;

        // A smaller chip ignores the top address bit: its second half is its first.
        const uint8_t low = 0x5A;
        const uint8_t high = 0xA5;
        uint8_t back[2] = {0, 0};
        write(0, &low, 1);
        write(_capacity / 2, &high, 1);
        read(0, &back[0], 1);
        read(_capacity / 2, &back[1], 1);
        return back[0] == low && back[1] == high;
    }

    uint32_t capacity() const { return _capacity; }

    bool read(uint32_t offset, uint8_t* buf, size_t len) override {
        if (offset > _capacity || len > _capacity - offset) return false;
        start(kRead, offset);
        // The chip ignores what is sent while it answers.
        _spi.transfer(buf, len);
        deselect();
        return true;
    }

    bool write(uint32_t offset, const uint8_t* buf, size_t len) override {
        if (offset > _capacity || len > _capacity - offset) return false;
        start(kWrite, offset);
        // transfer() overwrites what it sends, so the data goes through a copy.
        uint8_t chunk[32];
        while (len > 0) {
            const size_t n = len < sizeof(chunk) ? len : sizeof(chunk);
            memcpy(chunk, buf, n);
            _spi.transfer(chunk, n);
            buf += n;
            len -= n;
        }
        deselect();
        return true;
    }

private:
    enum : uint8_t {
        kRead       = 0x03,
        kWrite      = 0x02,
        kReadMode   = 0x05,
        kWriteMode  = 0x01,
        kSequential = 0x40,
        kModeMask   = 0xC0,
    };

    SPIClass& _spi;
    uint32_t  _capacity;
    uint32_t  _clock_hz;
    uint8_t   _cs;

    void select() {
        _spi.beginTransaction(SPISettings(_clock_hz, MSBFIRST, SPI_MODE0));
        digitalWrite(_cs, LOW);
    }

    void deselect() {
        digitalWrite(_cs, HIGH);
        _spi.endTransaction();
    }

    /// A command and its address: 24 bits on chips above 64 KB, else 16.
    void start(uint8_t command, uint32_t address) {
        select();
        _spi.transfer(command);
        if (_capacity > 65536UL) _spi.transfer(uint8_t(address >> 16));
        _spi.transfer(uint8_t(address >> 8));
        _spi.transfer(uint8_t(address));
    }
};
