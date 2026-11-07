// A storage of your own: two functions that read and write bytes at an
// offset, given to VMCustomStorage. Here they drive a 23LC1024 SPI SRAM by
// hand (MicroSwapSpiMemory.h has this ready, see the SpiMemory example);
// replace them with the calls of any memory you have.
//
// Wiring: 23LC1024 on the board's default SPI pins, chip select on SRAM_CS_PIN.

// The chip holds 128 KB, so the virtual memory must not be larger:
// VM_PAGE_SIZE * VM_PAGE_COUNT <= 131072. AVR's defaults (128 * 64) fit;
// on other boards use 128 pages of 1 KB. Must come before the include.
#if !defined(__AVR__)
#define VM_PAGE_SIZE  1024
#define VM_PAGE_COUNT 128
#endif

#include <MicroSwap.h>
#include <SPI.h>

const uint8_t SRAM_CS_PIN = SS; // change to your chip's chip select pin
const uint32_t SRAM_SIZE = 131072UL;

static_assert(uint32_t(VM_PAGE_SIZE) * VM_PAGE_COUNT <= SRAM_SIZE, "virtual memory is larger than the SRAM chip");

// A command (0x03 read, 0x02 write) and a 24-bit address, with chip select low.
static void startCommand(uint8_t command, uint32_t address) {
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(SRAM_CS_PIN, LOW);
    SPI.transfer(command);
    SPI.transfer(uint8_t(address >> 16));
    SPI.transfer(uint8_t(address >> 8));
    SPI.transfer(uint8_t(address));
}

static void endCommand() {
    digitalWrite(SRAM_CS_PIN, HIGH);
    SPI.endTransaction();
}

bool sramRead(void* context, uint32_t offset, uint8_t* buf, size_t len) {
    (void)context;
    if (offset + len > SRAM_SIZE) return false;
    startCommand(0x03, offset);
    for (size_t i = 0; i < len; ++i) buf[i] = SPI.transfer(0);
    endCommand();
    return true;
}

bool sramWrite(void* context, uint32_t offset, const uint8_t* buf, size_t len) {
    (void)context;
    if (offset + len > SRAM_SIZE) return false;
    startCommand(0x02, offset);
    for (size_t i = 0; i < len; ++i) SPI.transfer(buf[i]);
    endCommand();
    return true;
}

VMCustomStorage storage(sramRead, sramWrite);

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    pinMode(SRAM_CS_PIN, OUTPUT);
    digitalWrite(SRAM_CS_PIN, HIGH);
    SPI.begin();
    // Sequential mode, so a read or write runs on across the chip's own pages.
    SPI.beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
    digitalWrite(SRAM_CS_PIN, LOW);
    SPI.transfer(0x01);
    SPI.transfer(0x40);
    digitalWrite(SRAM_CS_PIN, HIGH);
    SPI.endTransaction();

    VMManager& vm = VMManager::instance();
    vm.begin(storage);

    VMVector<long> values;
    const long count = long(vm.get_page_size() / sizeof(long)) * 10; // 10 pages
    for (long i = 0; i < count; ++i) values.push_back(i * 3);

    long wrong = 0;
    for (long i = 0; i < count; ++i)
        if (values[i] != i * 3) wrong++;

    Serial.print(F("Checked "));
    Serial.print(count);
    Serial.print(F(" values through the SRAM, wrong: "));
    Serial.println(wrong); // 0 if the chip is wired correctly

    Serial.print(F("Pages swapped in: "));
    Serial.print(vm.swap_in_count());
    Serial.print(F(", out: "));
    Serial.println(vm.swap_out_count());
}

void loop() {}
