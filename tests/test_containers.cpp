// Host-side tests for MicroSwap.h. Build and run with `make -C tests`.
//
// The Makefile builds this file twice: with the default configuration, and
// with an AVR-like one (C++11, no STL, tiny pages) where pages are swapped
// out all the time.

#include "../src/MicroSwap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#if VM_USE_STL
#include <algorithm>
#include <numeric>
#endif

static int g_failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/// Swap storage in RAM. Reading data that was never written is reported as an error.
class RamStorage : public VMStorage {
public:
    bool fail_writes = false;
    int writes = 0;
    int flushes = 0;

    bool read(uint32_t offset, uint8_t* buf, size_t len) override {
        if (offset + len > _data.size()) return false;
        memcpy(buf, &_data[offset], len);
        return true;
    }

    bool write(uint32_t offset, const uint8_t* buf, size_t len) override {
        if (fail_writes) return false;
        // Like many file systems, refuse to write past the end of the file.
        if (offset > _data.size()) return false;
        if (offset + len > _data.size()) _data.resize(offset + len);
        memcpy(&_data[offset], buf, len);
        writes++;
        return true;
    }

    bool flush() override {
        flushes++;
        return true;
    }

private:
    std::vector<uint8_t> _data;
};

static RamStorage* g_storage = nullptr;
static int g_errors = 0;

/// Records errors instead of aborting, so tests can check them.
static void record_error(VMError) { g_errors++; }

static VMManager& vm() { return VMManager::instance(); }

static void reset_vm() {
    delete g_storage;
    g_storage = new RamStorage();
    vm().begin(*g_storage);
    vm().set_error_handler(record_error);
    g_errors = 0;
}

static size_t used_pages() { return vm().get_page_count() - vm().free_pages(); }

static const size_t INTS_PER_PAGE = VM_PAGE_SIZE / sizeof(int);

/// Element type that counts live instances, to catch leaks and double destruction.
struct Tracked {
    static int live;
    int value;
    Tracked(int v = 0) : value(v) { live++; }
    Tracked(const Tracked& o) : value(o.value) { live++; }
    Tracked(Tracked&& o) noexcept : value(o.value) { live++; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;
    ~Tracked() { live--; }
};
int Tracked::live = 0;

/// Object with a VMString member, like the README example.
struct Person {
    int age;
    VMString name;
    Person() : age(0) {}
    Person(int a, const char* n) : age(a), name(n) {}
};

// -----------------------------------------------------------------------------

static void test_data_survives_swapping() {
    // Several times more data than fits in RAM.
    const size_t n = INTS_PER_PAGE * (VM_RESIDENT_PAGES * 4);
    VMVector<int> v;
    for (size_t i = 0; i < n; ++i) v.push_back(int(i * 7));

    size_t wrong = 0;
    for (size_t i = 0; i < n; ++i)
        if (v[i] != int(i * 7)) wrong++;
    CHECK(wrong == 0);
    CHECK(vm().swap_out_count() > 0);
    CHECK(vm().swap_in_count() > 0);
    CHECK(g_errors == 0);
}

static void test_reserve_then_push_back() {
    const size_t n = INTS_PER_PAGE * 2;
    VMVector<int> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) v.push_back(int(i));

    size_t wrong = 0;
    for (size_t i = 0; i < n; ++i)
        if (v[i] != int(i)) wrong++;
    CHECK(wrong == 0);
}

static void test_small_vector_stays_flat() {
    VMVector<int> v;
    v.reserve(4);
    for (int i = 0; i < 4; ++i) v.push_back(i);
    CHECK(v.is_flat());
    CHECK(v.data() != nullptr);
    CHECK(v.back() == 3);
}

static void test_pop_back_across_chunk_boundary() {
    VMVector<int> v;
    for (size_t i = 0; i < INTS_PER_PAGE + 1; ++i) v.push_back(int(i));

    v.pop_back();
    CHECK(v.size() == INTS_PER_PAGE);
    CHECK(v.back() == int(INTS_PER_PAGE) - 1);

    v.push_back(777);
    CHECK(v[INTS_PER_PAGE] == 777);
}

static void test_pop_back_releases_pages() {
    VMVector<int> v;
    for (size_t i = 0; i < 3 * INTS_PER_PAGE; ++i) v.push_back(int(i));
    const size_t full = used_pages();

    while (!v.empty()) v.pop_back();
    CHECK(used_pages() < full);
}

static void test_insert_and_erase() {
    VMVector<int> v;
    for (int i = 0; i < 5; ++i) v.push_back(i);        // 0 1 2 3 4
    v.insert(v.begin() + 2, 99);                         // 0 1 99 2 3 4
    v.erase(v.begin());                                  // 1 99 2 3 4
    CHECK(v.size() == 5);
    CHECK(v[0] == 1 && v[1] == 99 && v[2] == 2 && v[4] == 4);
}

static void test_swap_and_move() {
    VMVector<int> small;
    small.push_back(5);
    VMVector<int> big;
    for (size_t i = 0; i < 2 * INTS_PER_PAGE; ++i) big.push_back(int(i));

    small.swap(big);
    CHECK(big.size() == 1 && big[0] == 5);
    CHECK(small.size() == 2 * INTS_PER_PAGE && small.back() == int(2 * INTS_PER_PAGE - 1));

    VMVector<int> moved(vm_detail::move(small));
    CHECK(small.empty());
    CHECK(moved.size() == 2 * INTS_PER_PAGE);
}

static void test_copy_vector() {
    VMVector<int> a;
    for (int i = 0; i < 10; ++i) a.push_back(i);
    VMVector<int> b(a);
    b[0] = 42;
    CHECK(a[0] == 0 && b[0] == 42);
    CHECK(b.size() == 10 && b[9] == 9);

    // Two ints mean "count, value", not an iterator range.
    b.assign(4, 1);
    CHECK(b.size() == 4 && b[0] == 1 && b[3] == 1);

    int raw[] = {5, 6, 7};
    b.assign(raw, raw + 3);
    CHECK(b.size() == 3 && b[2] == 7);
}

static void test_out_of_pages_reports_error() {
    VMVector<int> v;
    const size_t too_many = (VM_PAGE_COUNT + 1) * INTS_PER_PAGE;
    for (size_t i = 0; i < too_many && g_errors == 0; ++i) v.push_back(int(i));

    CHECK(g_errors > 0);
    CHECK(vm().last_error() == VMError::OutOfPages);
    CHECK(v.size() < too_many);
    CHECK(v[0] == 0); // existing elements are intact
}

static void test_out_of_range_reports_error() {
    VMVector<int> v;
    v.at(3);
    CHECK(vm().last_error() == VMError::OutOfRange);
}

static void test_no_leaks_for_non_trivial_elements() {
    {
        VMVector<Tracked> v;
        // Grows the flat block several times, then goes paged.
        const int n = int(INTS_PER_PAGE * 3);
        for (int i = 0; i < n; ++i) v.emplace_back(i);
        CHECK(Tracked::live == n);
        CHECK(v[n - 1].value == n - 1);

        v.pop_back();
        CHECK(Tracked::live == n - 1);
    }
    CHECK(Tracked::live == 0);
    CHECK(used_pages() == 0);
}

static void test_vector_of_objects_with_strings() {
    {
        VMVector<Person> people;
        for (int i = 0; i < 40; ++i) {
            char name[16];
            std::snprintf(name, sizeof(name), "person-%d", i);
            people.emplace_back(i, name);
        }
        CHECK(people[17].name == "person-17");
        CHECK(people[39].age == 39);
    }
    CHECK(used_pages() == 0);
}

static void test_heap_does_not_fragment() {
    // Growing strings reallocate repeatedly; without merging free blocks
    // the heap would eat every page.
    for (int round = 0; round < 300; ++round) {
        VMString s;
        const int len = (round * 7) % int(VMString::max_size());
        for (int k = 0; k < len; ++k) s.push_back('x');
    }
    CHECK(used_pages() == 0);
    CHECK(g_errors == 0);
}

static void test_vmstring_operations() {
    VMString s("hello");
    s.append(" world");
    CHECK(s == "hello world");

    s.replace(0, 5, "HELLO");
    CHECK(s == "HELLO world");

    s.insert(0, "[");
    s += ']';
    CHECK(s == "[HELLO world]");

    s.erase(1, 6);
    CHECK(s == "[world]");
    CHECK(s.find("orl") == 2);
    CHECK(s.rfind('o') == 2);
    CHECK(s.find('z') == VMString::npos);
    CHECK(s.find_first_of("dlr") == 3);
    CHECK(s.find_last_not_of("]") == 5);
    CHECK(s.substr(1, 5) == "world");

    VMString t = s + "!";
    CHECK(t == "[world]!");
    CHECK(t.compare("[world]") > 0);
}

static void test_vmstring_append_self() {
    VMString s("abc");
    s.append(s);
    CHECK(s == "abcabc");
    s.insert(1, s.c_str() + 4);
    CHECK(s == "abcbcabc");
}

static void test_vmstring_too_long() {
    VMString s;
    s.resize(VMString::max_size() + 1, 'x');
    CHECK(vm().last_error() == VMError::TooLarge);
    CHECK(s.empty());
}

static void test_empty_string_uses_no_memory() {
    VMString s;
    CHECK(s.c_str()[0] == '\0');
    CHECK(used_pages() == 0);
}

static void test_vmarray() {
    VMArray<int, 8> a;
    CHECK(used_pages() == 0); // allocated on first access
    CHECK(a[3] == 0);         // value-initialized
    for (size_t i = 0; i < a.size(); ++i) a[i] = int(i * i);

    VMArray<int, 8> b(a);
    b[2] = -1;
    CHECK(a[2] == 4 && b[2] == -1 && b[7] == 49);

    VMArray<Tracked, 4>* objects = new VMArray<Tracked, 4>();
    (*objects)[0].value = 1;
    CHECK(Tracked::live == 4);
    delete objects;
    CHECK(Tracked::live == 0);
}

static void test_vmptr() {
    VMPtr<Person> p = make_vm<Person>(28, "oscar");
    CHECK(p.valid());
    CHECK(p->age == 28 && p->name == "oscar");
    p->age++;
    CHECK((*p).age == 29);
    p.destroy();
    CHECK(!p.valid());
    CHECK(used_pages() == 0);

    VMPtr<int> null_ptr;
    *null_ptr = 1;
    CHECK(vm().last_error() == VMError::NullPointer);
}

static void test_storage_write_failure_reports_error() {
    VMVector<int> v;
    g_storage->fail_writes = true;
    for (size_t i = 0; i < INTS_PER_PAGE * (VM_RESIDENT_PAGES + 1); ++i) v.push_back(int(i));
    CHECK(vm().last_error() == VMError::StorageIO);
    g_storage->fail_writes = false;
}

static void test_reading_does_not_write_pages() {
    const size_t n = INTS_PER_PAGE * (VM_RESIDENT_PAGES * 3);
    VMVector<int> v;
    for (size_t i = 0; i < n; ++i) v.push_back(int(i));
    vm().flush_all();

    // Non-const access hands out references, but nothing is modified.
    const uint32_t outs = vm().swap_out_count();
    long long sum = 0;
    for (int round = 0; round < 3; ++round)
        for (size_t i = 0; i < n; ++i) sum += v[i];
    CHECK(sum == 3LL * (long long)n * (n - 1) / 2);
    CHECK(vm().swap_out_count() == outs);
    CHECK(vm().swap_in_count() > 0);
}

static void test_writes_through_references_are_saved() {
    const size_t n = INTS_PER_PAGE * (VM_RESIDENT_PAGES * 3);
    VMVector<int> v;
    for (size_t i = 0; i < n; ++i) v.push_back(int(i));
    for (size_t i = 0; i < n; ++i) v[i] += 1;

    size_t wrong = 0;
    for (size_t i = 0; i < n; ++i)
        if (v[i] != int(i) + 1) wrong++;
    CHECK(wrong == 0);
}

static void test_references_survive_counter_overflow() {
    // In the small configuration VM_TICK_LIMIT is low, so the access
    // counter is renumbered many times during this loop.
    VMVector<int> v;
    for (size_t i = 0; i < INTS_PER_PAGE * 8; ++i) v.push_back(int(i));

    size_t stale = 0;
    for (size_t k = 0; k < 5000; ++k) {
        const size_t a = (k % 8) * INTS_PER_PAGE;
        const size_t b = ((k * 3 + 1) % 8) * INTS_PER_PAGE;
        int& ref = v[a];
        (void)v[b]; // one other page: the reference must stay valid
        if (ref != int(a) || &ref != &v[a]) stale++;
    }
    CHECK(stale == 0);
}

static void test_storage_flushed_only_by_flush_all() {
    VMVector<int> v;
    for (size_t i = 0; i < INTS_PER_PAGE * (VM_RESIDENT_PAGES * 2); ++i) v.push_back(int(i));
    CHECK(g_storage->writes > 0);
    CHECK(g_storage->flushes == 0);
    CHECK(vm().flush_all());
    CHECK(g_storage->flushes == 1);
}

/// Pages that can be pinned at once in this configuration.
static const int PINNABLE = VM_RESIDENT_PAGES > 3 ? VM_RESIDENT_PAGES - 3 : 0;

/// Read every page of v, evicting everything that is not pinned. Reads go
/// through a const reference, so they never mark pages as possibly written.
static long long touch_all(const VMVector<int>& v) {
    long long sum = 0;
    for (size_t i = 0; i < v.size(); ++i) sum += v[i];
    return sum;
}

static void test_pin_keeps_reference_valid() {
    VMVector<int> v;
    for (size_t i = 0; i < INTS_PER_PAGE * (VM_RESIDENT_PAGES * 3); ++i) v.push_back(int(i));

    VMPin<int> first(v[0]);
    if (PINNABLE == 0) {
        CHECK(!first.valid());
        CHECK(vm().last_error() == VMError::TooManyPins);
        return;
    }
    CHECK(first.valid());

    touch_all(v);
    CHECK(*first == 0);
    const uint32_t loads = vm().swap_in_count();
    CHECK(&*first == &v[0]);                 // same RAM address...
    CHECK(vm().swap_in_count() == loads);    // ...and the page was not reloaded
    *first = 12345;

    first.release();
    touch_all(v);
    CHECK(v[0] == 12345);
    CHECK(g_errors == 0);
}

static void test_write_after_flush_is_saved() {
    VMVector<int> v;
    for (size_t i = 0; i < INTS_PER_PAGE * (VM_RESIDENT_PAGES * 3); ++i) v.push_back(int(i));

    int& ref = v[0];
    vm().flush_all();
    ref = 777; // written after the page was saved
    touch_all(v);
    CHECK(v[0] == 777);
}

static void test_too_many_pins() {
    VMVector<int> v;
    for (size_t i = 0; i < INTS_PER_PAGE * (VM_RESIDENT_PAGES + 2); ++i) v.push_back(int(i));

    std::vector<VMPin<int>> pins;
    int pinned = 0;
    for (int p = 0; p < VM_RESIDENT_PAGES; ++p) {
        pins.push_back(vm_pin(v[p * INTS_PER_PAGE]));
        if (pins.back().valid()) pinned++;
    }
    CHECK(pinned == PINNABLE);
    CHECK(vm().last_error() == VMError::TooManyPins);

    // A second pin on an already pinned page is always allowed.
    if (PINNABLE > 0) {
        VMPin<int> again(v[0]);
        CHECK(again.valid());
    }

    // With pins held, the containers still have room to work.
    vm().clear_error();
    g_errors = 0;
    CHECK(touch_all(v) > 0);
    CHECK(g_errors == 0);
}

static void test_pin_string_and_ptr() {
    if (PINNABLE == 0) return;
    VMString s("hello pinned world");
    VMPtr<Person> person = make_vm<Person>(30, "alice");

    VMVector<int> filler;
    for (size_t i = 0; i < INTS_PER_PAGE * (VM_RESIDENT_PAGES * 3); ++i) filler.push_back(int(i));

    {
        auto text = vm_pin(s.c_str());
        CHECK(text.valid());
        touch_all(filler);
        CHECK(strcmp(text.get(), "hello pinned world") == 0);
    }
    {
        VMPin<Person> p(*person);
        CHECK(p.valid());
        touch_all(filler);
        p->age++;
    }
    touch_all(filler);
    CHECK(person->age == 31);
    person.destroy();
}

static void test_pinned_nested_container_grows() {
    if (PINNABLE == 0) return;
    VMVector<VMVector<long> > rows;
    rows.emplace_back();
    rows.emplace_back();
    const long n = long(INTS_PER_PAGE) * long(VM_RESIDENT_PAGES) * 2; // goes paged
    {
        VMPin<VMVector<long> > row(rows[1]);
        CHECK(row.valid());
        for (long i = 0; i < n; ++i) row->push_back(i * 2);
    }
    CHECK(rows[0].empty());
    CHECK(rows[1].size() == size_t(n));
    size_t wrong = 0;
    for (long i = 0; i < n; ++i)
        if (rows[1][i] != i * 2) wrong++;
    CHECK(wrong == 0);
}

static void test_pin_outside_virtual_memory() {
    int local = 5;
    VMPin<int> p(local);
    CHECK(!p.valid());
    CHECK(*p == 5);
    CHECK(g_errors == 0);
}

#if VM_USE_STL
static void test_stl_algorithms() {
    // Enough elements to span more pages than fit in RAM.
    const int n = int(INTS_PER_PAGE * (VM_RESIDENT_PAGES + 2));
    VMVector<int> v;
    for (int i = 0; i < n; ++i) v.push_back((i * 7919) % n);

    std::sort(v.begin(), v.end());
    CHECK(std::is_sorted(v.begin(), v.end()));
    CHECK(std::accumulate(v.begin(), v.end(), 0LL) == (long long)n * (n - 1) / 2);

    std::reverse(v.begin(), v.end());
    CHECK(v.front() == n - 1);
    CHECK(std::find(v.begin(), v.end(), 5).pos() == size_t(n - 1 - 5));
}

static void test_sort_objects_with_strings() {
    VMVector<Person> people;
    const char* names[] = {"dave", "alice", "erin", "bob", "carol"};
    for (int round = 0; round < 20; ++round)
        for (int i = 0; i < 5; ++i) people.emplace_back(round * 5 + i, names[i]);

    std::sort(people.begin(), people.end(), [](const Person& a, const Person& b) {
        return a.name < b.name || (a.name == b.name && a.age < b.age);
    });
    CHECK(people.front().name == "alice");
    CHECK(people.back().name == "erin");

    bool ordered = true;
    for (size_t i = 1; i < people.size(); ++i)
        if (people[i].name < people[i - 1].name) ordered = false;
    CHECK(ordered);
}

static void test_string_algorithms() {
    VMString s("the quick brown fox");
    std::replace(s.begin(), s.end(), ' ', '_');
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return char(c >= 'a' && c <= 'z' ? c - 32 : c); });
    CHECK(s == "THE_QUICK_BROWN_FOX");
    CHECK(std::count(s.begin(), s.end(), '_') == 3);
}
#endif

int main() {
    // Unbuffered output shows which test was running if one crashes.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Config: page=%d resident=%d pages=%d stl=%d\n\n",
                VM_PAGE_SIZE, VM_RESIDENT_PAGES, VM_PAGE_COUNT, VM_USE_STL);

    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"data survives swapping", test_data_survives_swapping},
        {"reserve then push_back", test_reserve_then_push_back},
        {"small vector stays flat", test_small_vector_stays_flat},
        {"pop_back across chunk boundary", test_pop_back_across_chunk_boundary},
        {"pop_back releases pages", test_pop_back_releases_pages},
        {"insert and erase", test_insert_and_erase},
        {"swap and move", test_swap_and_move},
        {"copy vector", test_copy_vector},
        {"out of pages reports error", test_out_of_pages_reports_error},
        {"out of range reports error", test_out_of_range_reports_error},
        {"no leaks for non-trivial elements", test_no_leaks_for_non_trivial_elements},
        {"vector of objects with strings", test_vector_of_objects_with_strings},
        {"heap does not fragment", test_heap_does_not_fragment},
        {"VMString operations", test_vmstring_operations},
        {"VMString append self", test_vmstring_append_self},
        {"VMString too long", test_vmstring_too_long},
        {"empty string uses no memory", test_empty_string_uses_no_memory},
        {"VMArray", test_vmarray},
        {"VMPtr", test_vmptr},
        {"storage write failure reports error", test_storage_write_failure_reports_error},
        {"reading does not write pages", test_reading_does_not_write_pages},
        {"writes through references are saved", test_writes_through_references_are_saved},
        {"references survive counter overflow", test_references_survive_counter_overflow},
        {"storage flushed only by flush_all", test_storage_flushed_only_by_flush_all},
        {"pin keeps reference valid", test_pin_keeps_reference_valid},
        {"write after flush is saved", test_write_after_flush_is_saved},
        {"too many pins", test_too_many_pins},
        {"pin string and ptr", test_pin_string_and_ptr},
        {"pinned nested container grows", test_pinned_nested_container_grows},
        {"pin outside virtual memory", test_pin_outside_virtual_memory},
#if VM_USE_STL
        {"STL algorithms", test_stl_algorithms},
        {"sort objects with strings", test_sort_objects_with_strings},
        {"string algorithms", test_string_algorithms},
#endif
    };

    for (const Test& t : tests) {
        std::printf("%s\n", t.name);
        reset_vm();
        t.fn();
    }

    std::printf(g_failures ? "\n%d check(s) failed\n" : "\nAll tests passed\n", g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
