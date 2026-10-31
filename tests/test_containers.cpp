// Host-side tests for containers.h. Build and run with `make -C tests`.

#include <FS.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

// Tests inspect the page table, which VMManager keeps private.
// Every header containers.h needs is included above, so the macro
// only affects containers.h itself.
#define private public
#include "../containers.h"
#undef private

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static fs::FS g_fs;

static void reset_vm() {
    VMManager::instance().end();
    VMManager::instance().begin(g_fs, "/.swap");
}

static int allocated_pages() {
    int count = 0;
    for (const VMPage& pg : VMManager::instance().pages)
        if (pg.allocated) count++;
    return count;
}

static const size_t INTS_PER_PAGE = VM_PAGE_SIZE / sizeof(int);

/// Element type that counts live instances, to catch leaks and double destruction.
struct Tracked {
    static int live;
    int value;
    Tracked(int v = 0) : value(v) { live++; }
    Tracked(const Tracked& o) : value(o.value) { live++; }
    Tracked(Tracked&& o) noexcept : value(o.value) { live++; }
    Tracked& operator=(const Tracked&) = default;
    ~Tracked() { live--; }
};
int Tracked::live = 0;

static void test_reserve_then_push_back() {
    VMVector<int> v;
    v.reserve(2048);
    for (int i = 0; i < 2048; ++i) v.push_back(i);

    int wrong = 0;
    for (int i = 0; i < 2048; ++i)
        if (v[i] != i) wrong++;
    CHECK(wrong == 0);
    CHECK(v.size() == 2048);
}

static void test_reserve_keeps_flat_mode_when_it_fits() {
    VMVector<int> v;
    v.reserve(100);
    CHECK(v.is_flat());
    CHECK(v.capacity() >= 100);
    for (int i = 0; i < 100; ++i) v.push_back(i);
    CHECK(v.is_flat());
    CHECK(v.back() == 99);
}

static void test_pop_back_across_chunk_boundary() {
    VMVector<int> v;
    for (size_t i = 0; i < INTS_PER_PAGE + 1; ++i) v.push_back((int)i);

    v.pop_back();
    CHECK(v.size() == INTS_PER_PAGE);
    CHECK(v.back() == (int)INTS_PER_PAGE - 1);

    v.push_back(777);
    CHECK(v[INTS_PER_PAGE] == 777);
    CHECK(v[INTS_PER_PAGE - 1] == (int)INTS_PER_PAGE - 1);
}

static void test_pop_back_releases_pages() {
    VMVector<int> v;
    for (size_t i = 0; i < 3 * INTS_PER_PAGE; ++i) v.push_back((int)i);
    const int pages_full = allocated_pages();

    while (!v.empty()) v.pop_back();
    CHECK(allocated_pages() < pages_full);
}

static void test_swap_flat_vectors() {
    VMVector<int> a{1, 2, 3};
    VMVector<int> b{9};
    a.swap(b);

    CHECK(a.size() == 1 && a[0] == 9);
    CHECK(b.size() == 3 && b[0] == 1 && b[2] == 3);
}

static void test_swap_flat_with_paged() {
    VMVector<int> small{5};
    VMVector<int> big;
    for (size_t i = 0; i < 2 * INTS_PER_PAGE; ++i) big.push_back((int)i);

    small.swap(big);
    CHECK(big.size() == 1 && big[0] == 5);
    CHECK(small.size() == 2 * INTS_PER_PAGE && small.back() == (int)(2 * INTS_PER_PAGE - 1));
}

static void test_out_of_pages_throws() {
    VMVector<int> v;
    bool threw = false;
    try {
        for (size_t i = 0; i < (VM_PAGE_COUNT + 1) * INTS_PER_PAGE; ++i) v.push_back((int)i);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

static void test_no_leaks_for_non_trivial_elements() {
    {
        VMVector<Tracked> v;
        // Enough elements to grow the flat block several times and go paged.
        for (int i = 0; i < 2000; ++i) v.emplace_back(i);
        CHECK(Tracked::live == 2000);
        CHECK(v[1999].value == 1999);

        v.pop_back();
        CHECK(Tracked::live == 1999);
    }
    CHECK(Tracked::live == 0);
}

static void test_clear_after_pop_frees_flat_block() {
    VMVector<int> v{1, 2, 3};
    while (!v.empty()) v.pop_back();
    v.clear();
    CHECK(allocated_pages() == 0);
}

static void test_heap_does_not_fragment() {
    // Growing strings reallocate repeatedly; without merging free blocks
    // the heap would eat every page.
    for (int round = 0; round < 300; ++round) {
        VMString s;
        for (int k = 0; k < round * 10 % 4000; ++k) s.push_back('x');
    }
    CHECK(allocated_pages() == 0);
}

static void test_vmstring_basics() {
    VMString s("hello");
    s.append(" world");
    CHECK(s.compare("hello world") == 0);
    s.replace(0, 5, "HELLO");
    CHECK(s == VMString("HELLO world"));
}

int main() {
    // Unbuffered output shows which test was running if one crashes.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    struct { const char* name; void (*fn)(); } tests[] = {
        {"reserve then push_back", test_reserve_then_push_back},
        {"reserve keeps flat mode when it fits", test_reserve_keeps_flat_mode_when_it_fits},
        {"pop_back across chunk boundary", test_pop_back_across_chunk_boundary},
        {"pop_back releases pages", test_pop_back_releases_pages},
        {"swap flat vectors", test_swap_flat_vectors},
        {"swap flat with paged", test_swap_flat_with_paged},
        {"out of pages throws", test_out_of_pages_throws},
        {"no leaks for non-trivial elements", test_no_leaks_for_non_trivial_elements},
        {"clear after pop frees flat block", test_clear_after_pop_frees_flat_block},
        {"heap does not fragment", test_heap_does_not_fragment},
        {"VMString basics", test_vmstring_basics},
    };

    for (auto& t : tests) {
        std::printf("%s\n", t.name);
        reset_vm();
        t.fn();
    }

    std::printf(g_failures ? "\n%d check(s) failed\n" : "\nAll tests passed\n", g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
