// Just enough of the Arduino API for MicroSwapSpiSram.h on a desktop machine.
#pragma once

#include <stdint.h>
#include <string.h>

#define HIGH 1
#define LOW 0
#define OUTPUT 1

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t level);
