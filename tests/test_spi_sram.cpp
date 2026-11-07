// VMSpiSramStorage against a simulated 23LC-family chip (fake_arduino/):
// commands, addresses and chip select as the real chips take them.

#include "../src/MicroSwapSpiSram.h"
#include "fake_arduino/fake_sram.h"

#include <cstdio>

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

/// Fill a vector over many pages and a long string, read both back.
static void swap_through(VMSpiSramStorage& sram) {
    g_errors = 0;
    CHECK(vm().begin(sram));
    vm().set_error_handler(record_error);
    {
        VMVector<long> values;
        const long count = long(VM_PAGE_SIZE / sizeof(long)) * (VM_PAGE_COUNT / 2);
        for (long i = 0; i < count; ++i) values.push_back(i * 3 + 1);
        long wrong = 0;
        for (long i = 0; i < count; ++i) wrong += values[i] != i * 3 + 1;
        CHECK(wrong == 0);

        VMString text;
        for (int i = 0; i < 3 * int(VMString::max_flat_size()); ++i) text += char('a' + i % 26);
        int bad = 0;
        for (int i = 0; i < int(text.size()); ++i) bad += text[i] != char('a' + i % 26);
        CHECK(bad == 0 && !text.is_flat());
    }
    CHECK(vm().swap_out_count() > 0 && vm().swap_in_count() > 0);
    CHECK(g_errors == 0);
}

static void test_23lc1024() {
    g_sram.reset(131072, 3);
    VMSpiSramStorage sram(g_sram.cs_pin);
    CHECK(sram.begin());
    CHECK(g_sram.mode == 0x40);
    swap_through(sram);
    CHECK(g_sram.bytes_without_transaction == 0 && g_sram.bytes_outside_select == 0 && g_sram.bytes_not_sequential == 0);
}

static void test_23k256() {
    // 32 KB, 16-bit addresses.
    g_sram.reset(32768, 2);
    VMSpiSramStorage sram(g_sram.cs_pin, 32768);
    CHECK(sram.begin());
    swap_through(sram);
    CHECK(g_sram.bytes_outside_select == 0 && g_sram.bytes_not_sequential == 0);
}

static void test_no_chip() {
    g_sram.reset(131072, 3);
    g_sram.present = false;
    VMSpiSramStorage sram(g_sram.cs_pin);
    CHECK(!sram.begin());
}

static void test_chip_smaller_than_said() {
    // A 64 KB chip on 24-bit addresses, declared as 128 KB: its second half mirrors the first.
    g_sram.reset(65536, 3);
    VMSpiSramStorage sram(g_sram.cs_pin, 131072);
    CHECK(!sram.begin());
}

static void test_virtual_memory_too_big() {
    g_sram.reset(4096, 2);
    VMSpiSramStorage sram(g_sram.cs_pin, 4096);
    CHECK(uint32_t(VM_PAGE_SIZE) * VM_PAGE_COUNT > 4096);
    CHECK(!sram.begin());
}

static void test_bounds() {
    g_sram.reset(131072, 3);
    VMSpiSramStorage sram(g_sram.cs_pin);
    CHECK(sram.begin());
    uint8_t buf[4] = {1, 2, 3, 4};
    CHECK(sram.write(131068, buf, 4));
    CHECK(!sram.write(131069, buf, 4));
    CHECK(!sram.read(0xFFFFFFFFUL, buf, 2));
    uint8_t back[4] = {0, 0, 0, 0};
    CHECK(sram.read(131068, back, 4) && memcmp(back, buf, 4) == 0);
}

int main() {
    std::printf("Config: page=%d resident=%d pages=%d\n\n", VM_PAGE_SIZE, VM_RESIDENT_PAGES, VM_PAGE_COUNT);
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"23LC1024: containers swap through the chip", test_23lc1024},
        {"23K256: 16-bit addresses", test_23k256},
        {"no chip: begin() fails", test_no_chip},
        {"chip smaller than declared: begin() fails", test_chip_smaller_than_said},
        {"virtual memory larger than the chip: begin() fails", test_virtual_memory_too_big},
        {"reads and writes stay inside the chip", test_bounds},
    };
    for (const Test& t : tests) {
        std::printf("%s\n", t.name);
        t.fn();
    }
    std::printf(g_failures ? "\n%d check(s) failed\n" : "\nAll tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
