#pragma once

// Opens the swap file on an SD card with the right API for each board and
// starts MicroSwap. The same file is used by every example.
//
// Avoid the board's internal flash (LittleFS, SPIFFS) for swapping: it is
// rewritten constantly and wears out.

#include <MicroSwap.h>

#if defined(ESP8266) || (defined(ARDUINO_ARCH_RP2040) && !defined(ARDUINO_ARCH_MBED))
#include <SDFS.h>
#else
#include <SPI.h>
#include <SD.h>
#endif

#ifndef SD_CS_PIN
#define SD_CS_PIN SS // change to your SD card's chip select pin
#endif

static File swapFile;
static VMFileStorage<File> swapStorage(swapFile);

/// Mount the SD card, open the swap file for reading and writing, and start MicroSwap.
inline bool beginSwap(const char* path = "/swap.bin") {
#if defined(ESP8266) || (defined(ARDUINO_ARCH_RP2040) && !defined(ARDUINO_ARCH_MBED))
    SDFS.setConfig(SDFSConfig(SD_CS_PIN));
    if (!SDFS.begin()) return false;
    swapFile = SDFS.open(path, "w+");
#elif defined(ESP32)
    if (!SD.begin(SD_CS_PIN)) return false;
    swapFile = SD.open(path, "w+");
#else
    if (!SD.begin(SD_CS_PIN)) return false;
    SD.remove(path);
    // Not FILE_WRITE: it would append every write to the end of the file.
    swapFile = SD.open(path, O_READ | O_WRITE | O_CREAT);
#endif
    if (!swapFile) return false;
    return VMManager::instance().begin(swapStorage);
}

/// Like beginSwap(), but stops the sketch with a message if it fails.
inline void beginSwapOrHalt(const char* path = "/swap.bin") {
    if (beginSwap(path)) return;
    Serial.println(F("Cannot open the swap file on the SD card"));
    while (true) delay(1000);
}
