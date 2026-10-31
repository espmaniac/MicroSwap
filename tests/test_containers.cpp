// Host-side tests for MicroSwap.h. Build and run with `make -C tests`.
//
// The Makefile builds this file twice: with the default configuration, and
// with an AVR-like one (C++11, no STL, tiny pages) where pages are swapped
// out all the time.

// Count VM_YIELD() calls (see test_yields_on_every_page_transfer).
static unsigned long g_yields = 0;
#ifndef VM_YIELD
#define VM_YIELD() (++g_yields)
#endif

#include "../src/MicroSwap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
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
class RamStorage final : public VMStorage {
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

// ---- Global objects, constructed before main() ----
// The VMManager is zero-filled static memory, so it is valid even while
// other globals are being constructed, whatever order that happens in.

struct UsesManagerAtStartup {
    size_t free_pages;
    size_t page_count;
    VMError error;
    UsesManagerAtStartup()
        : free_pages(VMManager::instance().free_pages()),
          page_count(VMManager::instance().get_page_count()),
          error(VMManager::instance().last_error()) {}
};
static UsesManagerAtStartup g_startup;

static VMVector<int> g_global_vector;
static VMString g_global_string;
static VMArray<int, 4> g_global_array;

static RamStorage* g_storage = nullptr;
static int g_errors = 0;

/// Records errors instead of aborting, so tests can check them.
static void record_error(VMError e) { g_errors++; if (getenv("VM_TRACE_ERRORS")) std::printf("    error: %s\n", vm_error_name(e)); }

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

static void test_manager_usable_before_main() {
    CHECK(g_startup.free_pages == VM_PAGE_COUNT);
    CHECK(g_startup.page_count == VM_PAGE_COUNT);
    CHECK(g_startup.error == VMError::None);
}

static void test_global_containers() {
    for (int i = 0; i < 300; ++i) g_global_vector.push_back(i);
    g_global_string = "global";
    g_global_array[2] = 7;
    CHECK(g_global_vector[299] == 299);
    CHECK(g_global_string == "global");
    CHECK(g_global_array[2] == 7 && g_global_array[0] == 0);
    g_global_vector.clear();
    g_global_string.clear();
}

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

static void test_push_back_of_own_element() {
    // Growing a flat vector moves its elements; the argument is one of them.
    VMVector<int> numbers;
    numbers.push_back(42);
    int wrong = 0;
    for (int i = 0; i < 60; ++i) {
        numbers.push_back(numbers[0]);
        if (numbers.back() != 42) wrong++;
    }
    CHECK(wrong == 0);

    VMVector<VMString> strings;
    strings.emplace_back("first element");
    wrong = 0;
    for (int i = 0; i < 40; ++i) {
        if (i % 2) strings.push_back(strings[0]);
        else strings.emplace_back(strings[0]);
        if (!(strings.back() == "first element")) wrong++;
    }
    CHECK(wrong == 0);
    CHECK(strings[0] == "first element");

    // insert() builds the element at the end first, then moves it into place.
    VMVector<VMString> words;
    words.emplace_back("a");
    for (int i = 0; i < 20; ++i) words.insert(words.begin(), words[words.size() - 1]);
    wrong = 0;
    for (size_t i = 0; i < words.size(); ++i)
        if (!(words[i] == "a")) wrong++;
    CHECK(wrong == 0);
    CHECK(g_errors == 0);
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

static void test_vmstring_embedded_nul() {
    // '\0' inside the string is a character like any other, and never part
    // of a character set given as a C string.
    VMString s("ab");
    s.resize(4); // "ab\0\0"
    CHECK(s.size() == 4);
    CHECK(s.find_first_of("xyz") == VMString::npos);
    CHECK(s.find_last_of("xyz") == VMString::npos);
    CHECK(s.find_first_not_of("ab") == 2);
    CHECK(s.find_last_not_of("xyz") == 3);
    CHECK(s.find_first_of("b") == 1);
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
    // Enough accesses to renumber the LRU counter several times: with a
    // low VM_TICK_LIMIT, or with a 16-bit counter at its real limit.
    const size_t rounds = VM_TICK_LIMIT <= 0xFFFFUL ? size_t(VM_TICK_LIMIT) * 2 : 5000;
    VMVector<int> v;
    for (size_t i = 0; i < INTS_PER_PAGE * 8; ++i) v.push_back(int(i));

    size_t stale = 0;
    for (size_t k = 0; k < rounds; ++k) {
        const size_t a = (k % 8) * INTS_PER_PAGE;
        const size_t b = ((k * 3 + 1) % 8) * INTS_PER_PAGE;
        int& ref = v[a];
        (void)v[b]; // one other page: the reference must stay valid
        if (ref != int(a) || &ref != &v[a]) stale++;
    }
    CHECK(stale == 0);
}

static void test_yields_on_every_page_transfer() {
    // Every page read from or written to storage gives the system a turn
    // (on ESP8266 this keeps Wi-Fi and the watchdog alive).
    g_yields = 0;
    VMVector<int> v;
    for (size_t i = 0; i < INTS_PER_PAGE * (VM_RESIDENT_PAGES * 3); ++i) v.push_back(int(i));
    long long sum = 0;
    for (size_t i = 0; i < v.size(); ++i) sum += v[i];
    CHECK(sum > 0);
    CHECK(vm().swap_in_count() > 0 && vm().swap_out_count() > 0);
    CHECK(g_yields >= vm().swap_in_count() + vm().swap_out_count());
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
static const int PINNABLE = VM_RESIDENT_PAGES > VM_USER_PIN_RESERVE ? VM_RESIDENT_PAGES - VM_USER_PIN_RESERVE : 0;

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

// ---- Containers stored inside virtual memory ----
// Each operation must keep the page holding the container itself in RAM,
// even with only the minimum number of resident pages.

/// How many Person objects (with short names) fit `copies` times in virtual memory.
static int people_that_fit(int wanted, int copies) {
    const size_t per_person = sizeof(Person) + 16; // object plus its name block
    const size_t fit = size_t(VM_PAGE_COUNT) * VM_PAGE_SIZE / (per_person * size_t(copies) * 2);
    return int(vm_detail::min_of<size_t>(size_t(wanted), fit));
}

static void fill_people(VMVector<Person>& people, int n) {
    char name[16];
    for (int i = 0; i < n; ++i) {
        std::snprintf(name, sizeof(name), "p%d", i);
        people.emplace_back(i, name);
    }
}

static void test_append_to_string_inside_vector() {
    // As many people as comfortably fit in this configuration's virtual memory.
    const int n = int(vm_detail::min_of<size_t>(60, VM_PAGE_COUNT * VM_PAGE_SIZE / (8 * sizeof(Person))));
    VMVector<Person> people;
    fill_people(people, n);
    VMVector<VMString> suffixes;
    char text[16];
    for (int i = 0; i < n; ++i) {
        std::snprintf(text, sizeof(text), "-s%d", i);
        suffixes.emplace_back(text);
    }

    // Each append reallocates the name while reading another VM string.
    for (int round = 0; round < 3; ++round)
        for (int i = 0; i < n; ++i) people[i].name.append(suffixes[n - 1 - i]);

    int wrong = 0;
    for (int i = 0; i < n; ++i) {
        char want[64];
        std::snprintf(want, sizeof(want), "p%d-s%d-s%d-s%d", i, n - 1 - i, n - 1 - i, n - 1 - i);
        if (!(people[i].name == want) || people[i].age != i) wrong++;
    }
    CHECK(wrong == 0);
    CHECK(g_errors == 0);
}

static void test_reserve_inside_vector_of_vectors() {
    // Enough rows that the outer vector spans pages, within this configuration's memory.
    const size_t memory = size_t(VM_PAGE_COUNT) * VM_PAGE_SIZE;
    const int row_count = int(vm_detail::min_of<size_t>(60, memory / (4 * sizeof(VMVector<long>))));
    const int target = row_count * 2 / 3;
    VMVector<VMVector<long> > rows;
    for (int r = 0; r < row_count; ++r) rows.emplace_back();
    const long n = long(VM_PAGE_SIZE / sizeof(long)) * VM_RESIDENT_PAGES * 2;

    rows[target].reserve(size_t(n)); // allocates many pages in one call
    for (long i = 0; i < n; ++i) rows[target].push_back(i * 2);

    size_t wrong = 0;
    for (long i = 0; i < n; ++i)
        if (rows[target][size_t(i)] != i * 2) wrong++;
    CHECK(wrong == 0);
    CHECK(rows[target].size() == size_t(n));
    for (int r = 0; r < row_count; ++r)
        if (r != target) CHECK(rows[r].empty());
    CHECK(g_errors == 0);
}

static void test_compare_strings_inside_vector() {
    const int n = people_that_fit(40, 1);
    VMVector<Person> people;
    fill_people(people, n);

    // Selection sort by name, descending: compares names of two elements,
    // each in its own page, with their buffers in two more pages.
    for (size_t i = 0; i < people.size(); ++i)
        for (size_t j = i + 1; j < people.size(); ++j)
            if (people[i].name < people[j].name) vm_detail::swap(people[i], people[j]);

    bool ordered = true;
    for (size_t i = 1; i < people.size(); ++i)
        if (people[i - 1].name < people[i].name) ordered = false;
    CHECK(ordered);
    CHECK(people.back().name == "p0");
    CHECK(g_errors == 0);
}

static void test_copy_and_erase_objects_with_strings() {
    const int n = people_that_fit(50, 2);
    VMVector<Person> people;
    fill_people(people, n);

    VMVector<Person> copy(people); // copies every name from virtual memory
    copy.erase(copy.begin());      // move-assigns every element one place down
    copy.push_back(people[n / 5]); // copies an element of another VM vector

    char want[16];
    CHECK(copy.size() == size_t(n));
    CHECK(copy[0].name == "p1" && copy[0].age == 1);
    std::snprintf(want, sizeof(want), "p%d", n - 1);
    CHECK(copy[size_t(n - 2)].name == want);
    std::snprintf(want, sizeof(want), "p%d", n / 5);
    CHECK(copy[size_t(n - 1)].name == want && copy[size_t(n - 1)].age == n / 5);
    CHECK(people[0].name == "p0"); // the original is untouched
    CHECK(g_errors == 0);
}

static void test_three_levels_of_nesting() {
    // A vector of vectors of objects with strings needs one more page.
    if (VM_RESIDENT_PAGES < 5) return;
    VMVector<Person> source;
    fill_people(source, 40);
    VMVector<VMVector<Person> > groups;
    for (int g = 0; g < 20; ++g) groups.emplace_back();

    for (int i = 0; i < 40; ++i) groups[i % 20].push_back(source[i]);
    for (int g = 0; g < 20; ++g) groups[g][1].name.append("+");

    int wrong = 0;
    for (int i = 0; i < 40; ++i) {
        char want[16];
        std::snprintf(want, sizeof(want), "p%d%s", i, i >= 20 ? "+" : "");
        const Person& p = groups[i % 20][i / 20];
        if (p.age != i || !(p.name == want)) wrong++;
    }
    CHECK(wrong == 0);
    CHECK(g_errors == 0);
}

struct Pair {
    long a;
    long b;
    Pair(const long& x, const long& y) : a(x), b(y) {}
};

static void test_short_of_pages_reports_instead_of_corrupting() {
    // Pins: the user's pin, the vector being changed (it lives in virtual
    // memory) and both constructor arguments (each in a page of its own).
    // That leaves one unpinned page, but growing the vector needs two, so
    // growing must fail with TooManyPins and never move data incorrectly.
    // Pins as the default VM_USER_PIN_RESERVE of 4 allows.
    const int user_pin_count = VM_RESIDENT_PAGES - 4;
    if (user_pin_count <= 0) return;
    const size_t per_page = VM_PAGE_SIZE / sizeof(long);
    VMVector<long> xs, ys, zs;
    for (size_t i = 0; i < per_page * 2; ++i) {
        xs.push_back(long(i));
        ys.push_back(long(i) * 1000);
    }
    for (size_t i = 0; i < per_page * size_t(user_pin_count); ++i) zs.push_back(0);
    // Enough rows that the outer vector is on whole pages, apart from the
    // heap page that holds the inner vector's block.
    const size_t row_count = VM_PAGE_SIZE / sizeof(VMVector<Pair>) + 2;
    VMVector<VMVector<Pair> > rows;
    for (size_t r = 0; r < row_count; ++r) rows.emplace_back();
    const size_t target = row_count - 1;

    // Use every pin the configuration allows, each on a page of its own.
    std::vector<VMPin<long> > user_pins;
    for (int p = 0; p < user_pin_count; ++p) {
        user_pins.push_back(vm_pin(zs[size_t(p) * per_page]));
        CHECK(user_pins.back().valid());
    }

    std::vector<std::pair<long, long> > model;
    for (int k = 0; k < 40; ++k) {
        const size_t i = per_page + size_t(k) % per_page; // second page of xs
        const size_t before = rows[target].size();
        rows[target].emplace_back(xs[i], ys[i]);
        if (rows[target].size() > before) model.push_back(std::make_pair(long(i), long(i) * 1000));
    }
    user_pins.clear();

    CHECK(vm().last_error() == VMError::TooManyPins); // the case was reached
    CHECK(rows[target].size() == model.size());
    size_t wrong = 0;
    for (size_t k = 0; k < model.size(); ++k)
        if (rows[target][k].a != model[k].first || rows[target][k].b != model[k].second) wrong++;
    CHECK(wrong == 0);
}

#if VM_USER_PIN_RESERVE < 4
// ---- Operations with too few unpinned pages ----
// Built with a lower VM_USER_PIN_RESERVE, so the test can pin all pages but
// `free_pages`. Each operation that copies between two pages must then
// either work (two free) or report TooManyPins and change nothing (one free).

/// Pins every resident page except `free_pages`, using pages of a filler vector.
class PageHog {
public:
    explicit PageHog(int free_pages) {
        const size_t per_page = VM_PAGE_SIZE / sizeof(long);
        for (size_t i = 0; i < per_page * VM_RESIDENT_PAGES; ++i) _filler.push_back(0);
        for (int p = 0; p < VM_RESIDENT_PAGES - free_pages; ++p) {
            _pins.push_back(vm_pin(_filler[size_t(p) * per_page]));
            CHECK(_pins.back().valid());
        }
    }

private:
    VMVector<long> _filler; // declared first: destroyed after the pins
    std::vector<VMPin<long> > _pins;
};

static void make_paged(VMVector<long>& v, size_t pages, long base) {
    for (size_t i = 0; i < pages * (VM_PAGE_SIZE / sizeof(long)); ++i) v.push_back(base + long(i));
}

static bool equals(const VMVector<long>& v, const std::vector<long>& model) {
    if (v.size() != model.size()) return false;
    for (size_t i = 0; i < model.size(); ++i) if (v[i] != model[i]) return false;
    return true;
}

static std::vector<long> to_model(const VMVector<long>& v) {
    std::vector<long> m;
    for (size_t i = 0; i < v.size(); ++i) m.push_back(v[i]);
    return m;
}

static void scarce_case(int free_pages, int which) {
    reset_vm();
    VMVector<long> a, b;
    VMString text("a string with an existing block");
    VMArray<long, 4> array;
    array.fill(7);
    switch (which) {
        case 0: a.push_back(1); a.push_back(2); a.push_back(3); a.push_back(4); break; // flat, full
        case 1: case 2: case 3: make_paged(a, 3, 0); make_paged(b, 3, 0); break;
        default: break;
    }
    const std::vector<long> before = to_model(a);
    const std::string text_before = text.c_str();

    bool changed = false;
    {
        PageHog hog(free_pages);
        g_errors = 0;
        switch (which) {
            case 0: { // grow the flat block, or move to whole pages
                for (int k = 0; k < 40; ++k) a.push_back(100 + k);
                changed = a.size() != before.size();
                break;
            }
            case 1: a.erase(a.begin()); changed = a.size() != before.size(); break;
            case 2: a.insert(a.begin(), -1); changed = a.size() != before.size(); break;
            case 3: changed = (a == b); break;
            case 4: text.append(" and a lot more text so that it needs a larger block"); changed = text.size() != text_before.size(); break;
            default: { VMArray<long, 4> copy(array); changed = copy[0] == 7; break; }
        }
    }

    if (free_pages >= 2) {
        CHECK(changed);
        CHECK(g_errors == 0);
    } else {
        // Not enough pages: the error is reported and nothing is modified.
        CHECK(vm().last_error() == VMError::TooManyPins);
        if (which <= 2) CHECK(equals(a, before));
        if (which == 4) CHECK(text_before == text.c_str());
    }
}

static void test_too_few_unpinned_pages() {
    for (int which = 0; which <= 5; ++which) {
        scarce_case(1, which);
        scarce_case(2, which);
    }
}
#endif

static void test_pin_outside_virtual_memory() {
    int local = 5;
    VMPin<int> p(local);
    CHECK(!p.valid());
    CHECK(*p == 5);
    CHECK(g_errors == 0);
}

// ---- Model-based stress tests ----
// Random operations on VMVectors and on std::vectors side by side; after
// every operation the contents must match exactly. Elements are checked in
// forward, backward and random order to exercise every way of finding a page.

struct Rng {
    uint32_t state;
    explicit Rng(uint32_t seed) : state(seed) {}
    uint32_t next() { state = state * 1664525u + 1013904223u; return state >> 8; }
    size_t below(size_t n) { return n ? size_t(next() % n) : 0; }
};

/// Element counts that keep two vectors well within the virtual memory.
static size_t stress_capacity(size_t element_size) {
    const size_t per_page = VM_PAGE_SIZE / element_size;
    const size_t limit = VM_PAGE_COUNT * per_page / 5;
    return limit < 600 ? limit : 600;
}

template<typename T, typename Value>
static bool same_contents(const VMVector<T>& vm, const std::vector<Value>& model, Rng& rng, Value (*value_of)(const T&)) {
    if (vm.size() != model.size()) return false;
    const size_t n = model.size();
    switch (rng.below(3)) {
        case 0:
            for (size_t i = 0; i < n; ++i) if (value_of(vm[i]) != model[i]) return false;
            break;
        case 1:
            for (size_t i = n; i-- > 0;) if (value_of(vm[i]) != model[i]) return false;
            break;
        default:
            for (size_t k = 0; k < n; ++k) {
                const size_t i = rng.below(n);
                if (value_of(vm[i]) != model[i]) return false;
            }
    }
    return true;
}

static long long_value(const long& x) { return x; }
static int tracked_value(const Tracked& t) { return t.value; }

/// One random operation on (v, model), with `other` / `other_model` as a second pair.
template<typename T, typename Value>
static void random_operation(Rng& rng, size_t cap, VMVector<T>& v, std::vector<Value>& model,
                             VMVector<T>& other, std::vector<Value>& other_model) {
    const Value x = Value(rng.below(100000));
    const size_t n = model.size();
    switch (rng.below(16)) {
        case 0: case 1: case 2: case 3:
            if (n < cap) { v.push_back(T(x)); model.push_back(x); }
            break;
        case 4: case 5:
            if (n > 0) { v.pop_back(); model.pop_back(); }
            break;
        case 6:
            if (n < cap) {
                const size_t at = rng.below(n + 1);
                v.insert(v.begin() + at, T(x));
                model.insert(model.begin() + at, x);
            }
            break;
        case 7:
            if (n > 0) {
                const size_t at = rng.below(n);
                v.erase(v.begin() + at);
                model.erase(model.begin() + at);
            }
            break;
        case 8:
            if (n > 0) {
                const size_t at = rng.below(n);
                v[at] = T(x);
                model[at] = x;
            }
            break;
        case 9: {
            const size_t to = rng.below(cap + 1);
            v.resize(to, T(x));
            model.resize(to, x);
            break;
        }
        case 10:
            v.reserve(rng.below(cap + 1));
            break;
        case 11:
            v.shrink_to_fit();
            break;
        case 12:
            if (rng.below(4) == 0) { v.clear(); model.clear(); }
            break;
        case 13:
            v.swap(other);
            model.swap(other_model);
            break;
        case 14:
            if (rng.below(2) == 0) { v = other; model = other_model; }
            else { other = v; other_model = model; }
            break;
        default: {
            const size_t count = rng.below(cap / 2 + 1);
            v.assign(count, T(x));
            model.assign(count, x);
        }
    }
}

static void test_stress_vector_of_longs() {
    const size_t cap = stress_capacity(sizeof(long));
    for (uint32_t seed = 1; seed <= 4; ++seed) {
        Rng rng(seed);
        VMVector<long> a, b;
        std::vector<long> ma, mb;
        int mismatches = 0;
        for (int step = 0; step < 2500 && mismatches == 0; ++step) {
            if (rng.below(2)) random_operation(rng, cap, a, ma, b, mb);
            else random_operation(rng, cap, b, mb, a, ma);
            if (!same_contents(a, ma, rng, long_value) || !same_contents(b, mb, rng, long_value)) {
                std::printf("  seed %u: mismatch after step %d\n", seed, step);
                mismatches++;
            }
        }
        CHECK(mismatches == 0);
    }
    CHECK(used_pages() == 0);
    CHECK(g_errors == 0);
}

static void test_stress_vector_of_objects() {
    const size_t cap = stress_capacity(sizeof(Tracked));
    for (uint32_t seed = 11; seed <= 13; ++seed) {
        Rng rng(seed);
        {
            VMVector<Tracked> a, b;
            std::vector<int> ma, mb;
            int mismatches = 0;
            for (int step = 0; step < 2000 && mismatches == 0; ++step) {
                if (rng.below(2)) random_operation(rng, cap, a, ma, b, mb);
                else random_operation(rng, cap, b, mb, a, ma);
                if (!same_contents(a, ma, rng, tracked_value) || !same_contents(b, mb, rng, tracked_value)) {
                    std::printf("  seed %u: mismatch after step %d\n", seed, step);
                    mismatches++;
                }
                // Every element alive in a VMVector is counted exactly once.
                if (Tracked::live != int(ma.size() + mb.size())) {
                    std::printf("  seed %u: %d live objects, expected %d\n", seed, Tracked::live, int(ma.size() + mb.size()));
                    mismatches++;
                }
            }
            CHECK(mismatches == 0);
        }
        CHECK(Tracked::live == 0);
    }
    CHECK(used_pages() == 0);
    CHECK(g_errors == 0);
}

static void test_stress_vectors_inside_virtual_memory() {
    // The vectors themselves live in a VMVector, so their fields (including
    // the cached chunk) are in pages that are swapped in and out.
    const int kRows = 4;
    const size_t cap = stress_capacity(sizeof(long)) / kRows * 2;
    Rng rng(99);
    {
        VMVector<VMVector<long> > rows;
        for (int r = 0; r < kRows; ++r) rows.emplace_back();
        std::vector<std::vector<long> > models(kRows);
        const VMVector<VMVector<long> >& const_rows = rows;

        int mismatches = 0;
        for (int step = 0; step < 3000 && mismatches == 0; ++step) {
            const int r = int(rng.below(kRows));
            const int o = (r + 1 + int(rng.below(kRows - 1))) % kRows;
            if (rng.below(3) == 0) {
                // Read through a const reference: the row's page is only read.
                const VMVector<long>& row = const_rows[r];
                const size_t n = models[r].size();
                for (size_t k = 0; k < 4 && n > 0; ++k) {
                    const size_t i = rng.below(n);
                    if (const_rows[r][i] != models[r][i]) mismatches++;
                }
                if (row.size() != n) mismatches++;
            } else {
                random_operation(rng, cap, rows[r], models[r], rows[o], models[o]);
            }
            for (int k = 0; k < kRows; ++k)
                if (!same_contents(const_rows[k], models[k], rng, long_value)) {
                    std::printf("  row %d: mismatch after step %d\n", k, step);
                    mismatches++;
                }
        }
        CHECK(mismatches == 0);
    }
    CHECK(used_pages() == 0);
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
    std::printf("Config: page=%d resident=%d pages=%d stl=%d tick=%d bits\n\n",
                VM_PAGE_SIZE, VM_RESIDENT_PAGES, VM_PAGE_COUNT, VM_USE_STL, VM_TICK_BITS);

    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"manager usable before main", test_manager_usable_before_main},
        {"global containers", test_global_containers},
        {"data survives swapping", test_data_survives_swapping},
        {"reserve then push_back", test_reserve_then_push_back},
        {"small vector stays flat", test_small_vector_stays_flat},
        {"pop_back across chunk boundary", test_pop_back_across_chunk_boundary},
        {"pop_back releases pages", test_pop_back_releases_pages},
        {"push_back of own element", test_push_back_of_own_element},
        {"insert and erase", test_insert_and_erase},
        {"swap and move", test_swap_and_move},
        {"copy vector", test_copy_vector},
        {"out of pages reports error", test_out_of_pages_reports_error},
        {"out of range reports error", test_out_of_range_reports_error},
        {"no leaks for non-trivial elements", test_no_leaks_for_non_trivial_elements},
        {"vector of objects with strings", test_vector_of_objects_with_strings},
        {"heap does not fragment", test_heap_does_not_fragment},
        {"VMString operations", test_vmstring_operations},
        {"VMString embedded NUL", test_vmstring_embedded_nul},
        {"VMString append self", test_vmstring_append_self},
        {"VMString too long", test_vmstring_too_long},
        {"empty string uses no memory", test_empty_string_uses_no_memory},
        {"VMArray", test_vmarray},
        {"VMPtr", test_vmptr},
        {"storage write failure reports error", test_storage_write_failure_reports_error},
        {"reading does not write pages", test_reading_does_not_write_pages},
        {"writes through references are saved", test_writes_through_references_are_saved},
        {"references survive counter overflow", test_references_survive_counter_overflow},
        {"yields on every page transfer", test_yields_on_every_page_transfer},
        {"storage flushed only by flush_all", test_storage_flushed_only_by_flush_all},
        {"pin keeps reference valid", test_pin_keeps_reference_valid},
        {"write after flush is saved", test_write_after_flush_is_saved},
        {"too many pins", test_too_many_pins},
        {"pin string and ptr", test_pin_string_and_ptr},
        {"pinned nested container grows", test_pinned_nested_container_grows},
        {"pin outside virtual memory", test_pin_outside_virtual_memory},
        {"append to string inside vector", test_append_to_string_inside_vector},
        {"reserve inside vector of vectors", test_reserve_inside_vector_of_vectors},
        {"compare strings inside vector", test_compare_strings_inside_vector},
        {"copy and erase objects with strings", test_copy_and_erase_objects_with_strings},
        {"three levels of nesting", test_three_levels_of_nesting},
        {"short of pages reports instead of corrupting", test_short_of_pages_reports_instead_of_corrupting},
#if VM_USER_PIN_RESERVE < 4
        {"too few unpinned pages", test_too_few_unpinned_pages},
#endif
        {"stress: vector of longs", test_stress_vector_of_longs},
        {"stress: vector of objects", test_stress_vector_of_objects},
        {"stress: vectors inside virtual memory", test_stress_vectors_inside_virtual_memory},
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
