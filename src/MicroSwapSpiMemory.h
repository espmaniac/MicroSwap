#pragma once

/**
 * @file MicroSwapSpiMemory.h
 * @brief Swap storage on SPI memory chips: SRAM, PSRAM, FRAM, MRAM.
 *
 * A chip is described by a VMSpiChip: its size, address length, clock and
 * the few things in which chips differ. VMSpiChips has descriptions of
 * common chips; describe any other chip that reads and writes by address in
 * the same way:
 *
 *     #include <MicroSwapSpiMemory.h>
 *     VMSpiMemoryStorage memory(10, VMSpiChips::LC1024);   // chip select on pin 10
 *     void setup() {
 *         if (!memory.begin()) { ... }    // no chip, smaller than described, or too small
 *         VMManager::instance().begin(memory);
 *     }
 *
 * Flash and EEPROM chips are not made for this: they need erasing or slow
 * page writes and wear out under the constant rewrites of swapping.
 * The virtual memory (VM_PAGE_SIZE * VM_PAGE_COUNT) must fit in the chip.
 */

#include <Arduino.h>
#include <SPI.h>

#include "MicroSwap.h"

/// What a chip needs besides reading and writing (VMSpiChip::flags).
enum : uint8_t {
    kVMSpiModeRegister = 0x01, ///< Set sequential mode at start (Microchip 23LC/23K SRAM).
    kVMSpiWriteEnable  = 0x02, ///< Send write enable (0x06) before every write (FRAM, MRAM).
    kVMSpiReset        = 0x04, ///< Send reset enable and reset (0x66, 0x99) at start (PSRAM).
};

/// How a chip is read and written. Construct one for a chip VMSpiChips does not have.
struct VMSpiChip {
    uint32_t capacity;         ///< Bytes on the chip.
    uint8_t  address_bytes;    ///< Address length after a command: 2, 3 or 4.
    uint32_t max_clock_hz;     ///< Fastest SPI clock for reads and writes.
    uint8_t  flags;            ///< kVMSpiModeRegister, kVMSpiWriteEnable, kVMSpiReset.
    uint16_t max_select_us;    ///< Longest time chip select may stay low (PSRAM refresh), 0 for no limit.
    uint16_t page_bytes;       ///< A transfer must not cross a multiple of this (PSRAM pages), 0 for none.
    uint8_t  read_command;
    uint8_t  write_command;
    uint8_t  read_dummy_bytes; ///< Bytes between the address and the data of a read.

    constexpr VMSpiChip(uint32_t capacity_, uint8_t address_bytes_, uint32_t max_clock_hz_, uint8_t flags_ = 0,
                        uint16_t max_select_us_ = 0, uint16_t page_bytes_ = 0, uint8_t read_command_ = 0x03,
                        uint8_t write_command_ = 0x02, uint8_t read_dummy_bytes_ = 0)
        : capacity(capacity_), address_bytes(address_bytes_), max_clock_hz(max_clock_hz_), flags(flags_),
          max_select_us(max_select_us_), page_bytes(page_bytes_), read_command(read_command_),
          write_command(write_command_), read_dummy_bytes(read_dummy_bytes_) {}
};

/// Descriptions of common chips, from their datasheets.
namespace VMSpiChips {
// Microchip SPI SRAM.
constexpr VMSpiChip LC1024(131072UL, 3, 20000000UL, kVMSpiModeRegister); ///< 23LC1024 / 23A1024, 128 KB
constexpr VMSpiChip LC512(65536UL, 2, 20000000UL, kVMSpiModeRegister);   ///< 23LC512 / 23A512, 64 KB
constexpr VMSpiChip K256(32768UL, 2, 20000000UL, kVMSpiModeRegister);    ///< 23K256, 32 KB
constexpr VMSpiChip K640(8192UL, 2, 20000000UL, kVMSpiModeRegister);     ///< 23K640, 8 KB
// SPI PSRAM, 3.3 V: chip select at most 8 us low, 1 KB pages.
constexpr VMSpiChip APS6404(8388608UL, 3, 33000000UL, kVMSpiReset, 8, 1024); ///< APS6404L, ESP-PSRAM64H, LY68L6400, 8 MB
// FRAM: does not wear out and keeps its data without power.
constexpr VMSpiChip MB85RS64V(8192UL, 2, 20000000UL, kVMSpiWriteEnable);  ///< Fujitsu MB85RS64V, 8 KB
constexpr VMSpiChip FM25V10(131072UL, 3, 40000000UL, kVMSpiWriteEnable);  ///< Infineon FM25V10, 128 KB
} // namespace VMSpiChips

class VMSpiMemoryStorage : public VMStorage {
public:
    /**
     * @param cs_pin    chip select pin.
     * @param chip      the chip, from VMSpiChips or described by you.
     * @param clock_hz  SPI clock; 0 takes 8 MHz, or the chip's maximum if lower.
     *                  Long wires may need less; PSRAM is faster with more.
     */
    VMSpiMemoryStorage(uint8_t cs_pin, const VMSpiChip& chip, uint32_t clock_hz = 0, SPIClass& spi = SPI)
        : _spi(spi), _chip(chip), _clock_hz(clock_hz), _cs(cs_pin) {}

    /**
     * @brief Set the chip up and check it.
     * @return False if the description is invalid, if no chip answers, if it
     *         holds less than described (a smaller chip mirrors its first
     *         half), or if the virtual memory does not fit in it.
     */
    bool begin() {
        if (_clock_hz == 0) _clock_hz = 8000000UL;
        if (_clock_hz > _chip.max_clock_hz) _clock_hz = _chip.max_clock_hz;
        if (_chip.address_bytes < 2 || _chip.address_bytes > 4 || _chip.capacity < 2 || _clock_hz == 0) return false;
        if (uint32_t(VM_PAGE_SIZE) * VM_PAGE_COUNT > _chip.capacity) return false;
        _max_data = max_data_per_select();

        pinMode(_cs, OUTPUT);
        digitalWrite(_cs, HIGH);
        _spi.begin();

        if (_chip.flags & kVMSpiReset) {
            command(kResetEnable);
            command(kReset);
            delayMicroseconds(100);
        }
        if (_chip.flags & kVMSpiModeRegister) {
            // Sequential mode: reads and writes run on across the chip's own pages.
            select();
            _spi.transfer(kWriteMode);
            _spi.transfer(kSequential);
            deselect();
            select();
            _spi.transfer(kReadMode);
            const uint8_t mode = _spi.transfer(0xFF);
            deselect();
            if ((mode & kModeMask) != kSequential) return false;
        }

        // A missing chip reads as all ones or all zeros; a smaller one ignores
        // the top address bit, so its second half is its first.
        const uint8_t low[2] = {0x5A, 0xC3};
        const uint8_t high[2] = {0xA5, 0x3C};
        uint8_t back_low[2] = {0, 0};
        uint8_t back_high[2] = {0, 0};
        const uint32_t half = _chip.capacity / 2;
        write(0, low, 2);
        write(half, high, 2);
        read(0, back_low, 2);
        read(half, back_high, 2);
        return memcmp(back_low, low, 2) == 0 && memcmp(back_high, high, 2) == 0;
    }

    const VMSpiChip& chip() const { return _chip; }
    uint32_t capacity() const { return _chip.capacity; }

    bool read(uint32_t offset, uint8_t* buf, size_t len) override {
        if (!inside(offset, len)) return false;
        while (len > 0) {
            const size_t n = piece(offset, len);
            start(_chip.read_command, offset);
            for (uint8_t i = 0; i < _chip.read_dummy_bytes; ++i) _spi.transfer(0xFF);
            // The chip ignores what is sent while it answers.
            _spi.transfer(buf, n);
            deselect();
            offset += n;
            buf += n;
            len -= n;
        }
        return true;
    }

    bool write(uint32_t offset, const uint8_t* buf, size_t len) override {
        if (!inside(offset, len)) return false;
        while (len > 0) {
            const size_t n = piece(offset, len);
            if (_chip.flags & kVMSpiWriteEnable) command(kWriteEnable);
            start(_chip.write_command, offset);
            // transfer() overwrites what it sends, so the data goes through a copy.
            uint8_t chunk[32];
            for (size_t done = 0; done < n;) {
                const size_t k = n - done < sizeof(chunk) ? n - done : sizeof(chunk);
                memcpy(chunk, buf + done, k);
                _spi.transfer(chunk, k);
                done += k;
            }
            deselect();
            offset += n;
            buf += n;
            len -= n;
        }
        return true;
    }

private:
    enum : uint8_t {
        kWriteMode   = 0x01,
        kReadMode    = 0x05,
        kWriteEnable = 0x06,
        kResetEnable = 0x66,
        kReset       = 0x99,
        kSequential  = 0x40,
        kModeMask    = 0xC0,
    };

    SPIClass& _spi;
    VMSpiChip _chip;
    uint32_t  _clock_hz;
    size_t    _max_data = 0; ///< Data bytes per chip select, 0 for no limit.
    uint8_t   _cs;

    bool inside(uint32_t offset, size_t len) const { return offset <= _chip.capacity && len <= _chip.capacity - offset; }

    /**
     * Data bytes that keep chip select under max_select_us at this clock,
     * with half the time left for the gaps between bytes. At least one.
     */
    size_t max_data_per_select() const {
        if (_chip.max_select_us == 0) return 0;
        const uint32_t bytes = uint32_t((uint64_t)_chip.max_select_us * _clock_hz / 8000000UL / 2);
        const uint32_t header = 1U + _chip.address_bytes + _chip.read_dummy_bytes;
        return bytes > header ? bytes - header : 1;
    }

    /// The part of a transfer at offset that one chip select may carry.
    size_t piece(uint32_t offset, size_t len) const {
        size_t n = len;
        if (_chip.page_bytes != 0) {
            const size_t to_page_end = _chip.page_bytes - offset % _chip.page_bytes;
            if (n > to_page_end) n = to_page_end;
        }
        if (_max_data != 0 && n > _max_data) n = _max_data;
        return n;
    }

    void select() {
        _spi.beginTransaction(SPISettings(_clock_hz, MSBFIRST, SPI_MODE0));
        digitalWrite(_cs, LOW);
    }

    void deselect() {
        digitalWrite(_cs, HIGH);
        _spi.endTransaction();
    }

    void command(uint8_t code) {
        select();
        _spi.transfer(code);
        deselect();
    }

    /// A command and its address, most significant byte first.
    void start(uint8_t code, uint32_t address) {
        select();
        _spi.transfer(code);
        for (int shift = 8 * (_chip.address_bytes - 1); shift >= 0; shift -= 8)
            _spi.transfer(uint8_t(address >> shift));
    }
};
