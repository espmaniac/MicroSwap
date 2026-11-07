// A simulated SPI bus with one memory chip on it (see fake_memory.h).
#pragma once

#include "Arduino.h"

#define MSBFIRST 1
#define SPI_MODE0 0

struct SPISettings {
    SPISettings(uint32_t clock_, uint8_t, uint8_t) : clock(clock_) {}
    uint32_t clock;
};

class SPIClass {
public:
    void begin() {}
    void beginTransaction(SPISettings settings) {
        in_transaction = true;
        clock = settings.clock;
    }
    void endTransaction() { in_transaction = false; }
    uint8_t transfer(uint8_t out);
    void transfer(void* buf, size_t count) {
        uint8_t* p = static_cast<uint8_t*>(buf);
        for (size_t i = 0; i < count; ++i) p[i] = transfer(p[i]);
    }
    bool in_transaction = false;
    uint32_t clock = 0;
};

extern SPIClass SPI;
