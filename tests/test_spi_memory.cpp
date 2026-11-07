// VMSpiMemoryStorage against a simulated chip of each family (fake_arduino/),
// and VMCustomStorage on functions.

#include "../src/MicroSwapSpiMemory.h"
#include "fake_arduino/fake_memory.h"

#include <cstdio>
#include <vector>

static int g_failures = 0;
static int g_errors = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static void record_error(VMError) { g_errors++; }

static VMManager& vm() { return VMManager::instance(); }

/// Fill a vector over many pages and a long string, read both back; 0 when all is right.
static int swap_through(VMStorage& storage) {
    int wrong = 0;
    g_errors = 0;
    if (!vm().begin(storage)) return -1;
    vm().set_error_handler(record_error);
    {
        VMVector<long> values;
        const long count = long(VM_PAGE_SIZE / sizeof(long)) * (VM_PAGE_COUNT / 2);
        for (long i = 0; i < count; ++i) values.push_back(i * 3 + 1);
        for (long i = 0; i < count; ++i) wrong += values[i] != i * 3 + 1;

        VMString text;
        for (int i = 0; i < 3 * int(VMString::max_flat_size()); ++i) text += char('a' + i % 26);
        for (int i = 0; i < int(text.size()); ++i) wrong += text[i] != char('a' + i % 26);
    }
    if (vm().swap_out_count() == 0 || vm().swap_in_count() == 0) wrong++;
    return wrong + g_errors;
}

static void sram(size_t size, int address_bytes) {
    g_chip.reset(size, address_bytes);
    g_chip.mode_register = true;
}

static void fram(size_t size, int address_bytes) {
    g_chip.reset(size, address_bytes);
    g_chip.write_enable = true;
    g_chip.max_clock = 40000000;
}

static void psram(uint32_t clock) {
    g_chip.reset(8388608, 3);
    g_chip.page_bytes = 1024;
    g_chip.max_clock = 33000000;
    // 8 us of chip select at this clock, less the command and address.
    g_chip.max_data_per_select = clock / 1000000 - 4;
}

static void test_sram_chips() {
    const VMSpiChip chips[] = {VMSpiChips::LC1024, VMSpiChips::LC512, VMSpiChips::K256};
    for (const VMSpiChip& chip : chips) {
        sram(chip.capacity, chip.address_bytes);
        VMSpiMemoryStorage memory(g_chip.cs_pin, chip);
        CHECK(memory.begin());
        CHECK(g_chip.mode == 0x40);
        CHECK(swap_through(memory) == 0);
        CHECK(g_chip.faults() == 0);
    }
}

static void test_fram_chips() {
    const VMSpiChip chips[] = {VMSpiChips::MB85RS64V, VMSpiChips::FM25V10};
    for (const VMSpiChip& chip : chips) {
        fram(chip.capacity, chip.address_bytes);
        VMSpiMemoryStorage memory(g_chip.cs_pin, chip, 20000000UL);
        CHECK(memory.begin());
        CHECK(swap_through(memory) == 0);
        CHECK(g_chip.faults() == 0);
    }
}

static void test_psram() {
    for (uint32_t clock : {8000000UL, 33000000UL}) {
        psram(clock);
        VMSpiMemoryStorage memory(g_chip.cs_pin, VMSpiChips::APS6404, clock);
        CHECK(memory.begin());
        CHECK(g_chip.resets == 1);
        CHECK(swap_through(memory) == 0);
        CHECK(g_chip.faults() == 0);

        // A transfer across a 1 KB page boundary is split there.
        uint8_t out[40];
        uint8_t back[40];
        for (int i = 0; i < 40; ++i) out[i] = uint8_t(i * 5 + 1);
        CHECK(memory.write(1024 - 20, out, 40));
        CHECK(memory.read(1024 - 20, back, 40));
        CHECK(memcmp(out, back, 40) == 0);
        CHECK(g_chip.faults() == 0);
    }
}

static void test_own_chip() {
    // A chip no list has: 4-byte addresses, fast read with one dummy byte.
    g_chip.reset(65536, 4);
    g_chip.read_cmd = 0x0B;
    g_chip.read_dummy = 1;
    const VMSpiChip mine(65536UL, 4, 20000000UL, 0, 0, 0, 0x0B, 0x02, 1);
    VMSpiMemoryStorage memory(g_chip.cs_pin, mine);
    CHECK(memory.begin());
    CHECK(swap_through(memory) == 0);
    CHECK(g_chip.faults() == 0);
}

static void test_clock() {
    // The default is 8 MHz, never more than the chip takes.
    fram(8192, 2);
    g_chip.max_clock = 5000000;
    VMSpiMemoryStorage slow(g_chip.cs_pin, VMSpiChip(8192UL, 2, 5000000UL, kVMSpiWriteEnable));
    CHECK(slow.begin());
    CHECK(SPI.clock == 5000000);
    CHECK(g_chip.clock_too_fast == 0);
}

static void test_wrong_descriptions_are_caught() {
    // FRAM described without write enable: the chip ignores the writes.
    fram(8192, 2);
    VMSpiMemoryStorage no_enable(g_chip.cs_pin, VMSpiChip(8192UL, 2, 20000000UL));
    CHECK(!no_enable.begin());
    CHECK(g_chip.writes_without_enable > 0);

    // PSRAM described without its page and chip select limits: data wraps and is lost.
    psram(33000000UL);
    VMSpiMemoryStorage no_limits(g_chip.cs_pin, VMSpiChip(8388608UL, 3, 33000000UL, kVMSpiReset), 33000000UL);
    CHECK(no_limits.begin());
    CHECK(swap_through(no_limits) != 0);
    CHECK(g_chip.page_crossings > 0 || g_chip.selects_too_long > 0);
}

static void test_begin_refuses() {
    sram(131072, 3);
    g_chip.present = false;
    VMSpiMemoryStorage absent(g_chip.cs_pin, VMSpiChips::LC1024);
    CHECK(!absent.begin());

    // A 64 KB chip on 24-bit addresses, described as 128 KB: its second half mirrors the first.
    sram(65536, 3);
    VMSpiMemoryStorage smaller(g_chip.cs_pin, VMSpiChips::LC1024);
    CHECK(!smaller.begin());

    sram(4096, 2);
    VMSpiMemoryStorage tiny(g_chip.cs_pin, VMSpiChip(4096UL, 2, 20000000UL, kVMSpiModeRegister));
    CHECK(uint32_t(VM_PAGE_SIZE) * VM_PAGE_COUNT > 4096);
    CHECK(!tiny.begin());

    sram(131072, 3);
    VMSpiMemoryStorage bad(g_chip.cs_pin, VMSpiChip(131072UL, 5, 20000000UL));
    CHECK(!bad.begin());
}

static void test_bounds() {
    sram(131072, 3);
    VMSpiMemoryStorage memory(g_chip.cs_pin, VMSpiChips::LC1024);
    CHECK(memory.begin());
    uint8_t buf[4] = {1, 2, 3, 4};
    CHECK(memory.write(131068, buf, 4));
    CHECK(!memory.write(131069, buf, 4));
    CHECK(!memory.read(0xFFFFFFFFUL, buf, 2));
    uint8_t back[4] = {0, 0, 0, 0};
    CHECK(memory.read(131068, back, 4) && memcmp(back, buf, 4) == 0);
}

// ---- VMCustomStorage ----

struct Ram {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(VM_PAGE_SIZE * VM_PAGE_COUNT);
    int flushes = 0;
};

static bool ram_read(void* ctx, uint32_t offset, uint8_t* buf, size_t len) {
    Ram& r = *static_cast<Ram*>(ctx);
    if (offset + len > r.bytes.size()) return false;
    memcpy(buf, &r.bytes[offset], len);
    return true;
}

static bool ram_write(void* ctx, uint32_t offset, const uint8_t* buf, size_t len) {
    Ram& r = *static_cast<Ram*>(ctx);
    if (offset + len > r.bytes.size()) return false;
    memcpy(&r.bytes[offset], buf, len);
    return true;
}

static bool ram_flush(void* ctx) {
    static_cast<Ram*>(ctx)->flushes++;
    return true;
}

static void test_custom_storage() {
    Ram ram;
    VMCustomStorage storage(ram_read, ram_write, &ram, ram_flush);
    CHECK(swap_through(storage) == 0);
    {
        VMVector<long> v;
        for (long i = 0; i < 100; ++i) v.push_back(i);
        CHECK(vm().flush_all());
    }
    CHECK(ram.flushes == 1);

    VMCustomStorage no_flush(ram_read, ram_write, &ram);
    CHECK(no_flush.flush());
    VMCustomStorage nothing(nullptr, nullptr);
    uint8_t byte = 0;
    CHECK(!nothing.read(0, &byte, 1) && !nothing.write(0, &byte, 1));
}

int main() {
    std::printf("Config: page=%d resident=%d pages=%d\n\n", VM_PAGE_SIZE, VM_RESIDENT_PAGES, VM_PAGE_COUNT);
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"SRAM: 23LC1024, 23LC512, 23K256", test_sram_chips},
        {"FRAM: MB85RS64V, FM25V10 (write enable)", test_fram_chips},
        {"PSRAM: APS6404 (reset, pages, chip select time) at 8 and 33 MHz", test_psram},
        {"a chip described by hand: 4-byte addresses, fast read", test_own_chip},
        {"clock: never above the chip's", test_clock},
        {"wrong descriptions are caught", test_wrong_descriptions_are_caught},
        {"begin() refuses: no chip, smaller, too small, bad description", test_begin_refuses},
        {"reads and writes stay inside the chip", test_bounds},
        {"VMCustomStorage on functions", test_custom_storage},
    };
    for (const Test& t : tests) {
        std::printf("%s\n", t.name);
        t.fn();
    }
    std::printf(g_failures ? "\n%d check(s) failed\n" : "\nAll tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
