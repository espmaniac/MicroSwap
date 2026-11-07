// A simulated SPI bus with a Microchip 23LC-family SRAM on it (see fake_sram.h).
#pragma once

#include "Arduino.h"

#define MSBFIRST 1
#define SPI_MODE0 0

struct SPISettings {
    SPISettings(uint32_t, uint8_t, uint8_t) {}
};

class SPIClass {
public:
    void begin() {}
    void beginTransaction(SPISettings) { in_transaction = true; }
    void endTransaction() { in_transaction = false; }
    uint8_t transfer(uint8_t out);
    void transfer(void* buf, size_t count) {
        uint8_t* p = static_cast<uint8_t*>(buf);
        for (size_t i = 0; i < count; ++i) p[i] = transfer(p[i]);
    }
    bool in_transaction = false;
};

extern SPIClass SPI;
