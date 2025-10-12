> Note: This codebase and README were written entirely by GitHub Copilot.

# MicroSwap — Arduino virtual memory with swap + STL-like containers

MicroSwap is an Arduino/ESP library that provides a lightweight virtual memory manager with paging/swap to a file (SD/SPIFFS/LittleFS) and a set of STL-like containers that store their data in this virtual memory:
- VMVector<T> — vector with hybrid flat/paged storage
- VMArray<T, N> — fixed-size array with proper object lifetime
- VMString — mutable string stored in the small-block heap
- VMPtr<T> — smart pointer to an object in virtual memory
- make_vm<T>(...) — factory to create VMPtr-managed objects safely (no placement new in user code)

Perfect for projects short on RAM where some data can be paged out to a swap file when inactive.

## Features
- Fixed number of pages (size configured by compile-time constants)
- Lazy on-demand page swap-in on access
- Dirty page tracking and explicit flushing
- STL-like containers with iterators and compatibility with standard algorithms
- Shared small-block heap so multiple small objects/strings can share pages
- VMVector hybrid storage:
  - Flat mode: single contiguous small-heap block with data()
  - Paged mode: grows beyond single-block capacity; data() becomes unavailable (nullptr)
- VMArray: automatically constructs/destructs non-trivial types; zero-initializes trivial types
- VMString: single-block design on the small heap
- VMPtr: smart pointer to VM object; construct with make_vm<T>(...) (no placement new in user code)

## Requirements
- Arduino Core (ESP32/ESP8266 or compatible)
- An Arduino FS implementation (SD/SPIFFS/LittleFS)
- C++17

## Installation
- Copy the library folder into your Arduino libraries/ directory, or add to your PlatformIO project
- Include SD.h (or a compatible FS header) and configure your SD/FS pins

## Quick start
```cpp
#include <Arduino.h>
#include <SD.h>
#include "containers.h"

static constexpr int SD_CS_PIN = 5;
static const char* SWAP_PATH = "/.swap";

void setup() {
  Serial.begin(115200);
  SD.begin(SD_CS_PIN);

  if (!VMManager::instance().begin(SD, SWAP_PATH)) {
    Serial.println("VM init failed");
    while (true) delay(1000);
  }

  VMVector<int> v;
  v.push_back(1);
  v.push_back(2);
  v.push_back(3);

  Serial.println(v.at(1)); // 2

  VMManager::instance().flush_all();
}

void loop() {}
```

## Full example (no placement new)
The sketch below demonstrates VMString, VMVector<int>, VMArray<int, N>, VMVector<Person> with push_back, and VMPtr<Person> via make_vm — all without using placement new in user code.

```cpp
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// STL headers for algorithms and helpers
#include <algorithm>
#include <numeric>
#include <cctype>
#include <cstring>

// MicroSwap containers
#include "containers.h"

// SD Card CS pin
static constexpr int SD_CS_PIN = 5;
// Swap file path on SD
static const char* SWAP_PATH = "/.swap";

/**
 * @brief Non-trivial class example: has constructors, methods, copy/move operations, and a VMString field.
 *
 * @details
 *  - Demonstrates storing objects with dynamic members (VMString) inside VMVector/VMArray/VMPtr.
 *  - Provides methods to mutate state and accessors for algorithms.
 */
class Person {
public:
  /// Default constructor
  Person() : _age(0), _name("UNKNOWN") {}

  /// Construct from age and C-string name
  Person(int age, const char* name) : _age(age), _name(name) {}

  /// Copy constructor
  Person(const Person& other) : _age(other._age), _name(other._name) {}

  /// Copy assignment
  Person& operator=(const Person& other) {
    if (this != &other) {
      _age = other._age;
      _name = other._name;
    }
    return *this;
  }

  /// Move constructor
  Person(Person&& other) noexcept : _age(other._age), _name(std::move(other._name)) {}

  /// Move assignment
  Person& operator=(Person&& other) noexcept {
    if (this != &other) {
      _age = other._age;
      _name = std::move(other._name);
    }
    return *this;
  }

  /// Destructor (non-trivial type marker; nothing special here)
  ~Person() {}

  /// Increase age by delta (default = 1)
  void age_up(int delta = 1) { _age += delta; }

  /// Uppercase the name in-place
  void upper_name() {
    for (size_t i = 0; i < _name.size(); ++i) {
      char c = _name[i];
      _name[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
  }

  /// Accessors
  int age() const { return _age; }
  const VMString& name() const { return _name; }

  /// Comparators for algorithms
  static bool by_age_asc(const Person& a, const Person& b) { return a._age < b._age; }
  static bool by_name_asc(const Person& a, const Person& b) { return a._name.compare(b._name) < 0; }

private:
  int _age;        ///< Age attribute (as an example numeric field).
  VMString _name;  ///< Name stored in VMString (dynamic small-heap backed).
};

/**
 * @brief Print fatal error and halt.
 * @param msg Message.
 */
static void fatal(const char* msg) {
  Serial.println(msg);
  Serial.flush();
  while (true) { delay(1000); }
}

/**
 * @brief Pretty-print a VMVector<int> (first and last few elements).
 * @param v Vector to print.
 * @param label Prefix label.
 */
static void print_vector_preview(const VMVector<int>& v, const char* label) {
  Serial.print(label);
  Serial.print(F(" (size="));
  Serial.print(v.size());
  Serial.println(F("):"));

  size_t n = v.size();
  size_t head = std::min<size_t>(n, 8);
  for (size_t i = 0; i < head; ++i) {
    Serial.print(v[i]);
    Serial.print(i + 1 < head ? F(", ") : F(""));
  }
  if (n > head) {
    Serial.print(F(" ... "));
    size_t tail = std::min<size_t>(8, n);
    for (size_t i = n - tail; i < n; ++i) {
      Serial.print(v[i]);
      Serial.print(i + 1 < n ? F(", ") : F(""));
    }
  }
  Serial.println();
}

/**
 * @brief Print a single Person.
 * @param p Person to print.
 */
static void print_person(const Person& p) {
  Serial.print(F("{age="));
  Serial.print(p.age());
  Serial.print(F(", name="));
  Serial.print(p.name().c_str());
  Serial.print(F("}"));
}

/**
 * @brief Pretty-print a VMVector<Person> (first and last few elements).
 * @param v Vector to print.
 * @param label Prefix label.
 */
static void print_people_preview(const VMVector<Person>& v, const char* label) {
  Serial.print(label);
  Serial.print(F(" (size="));
  Serial.print(v.size());
  Serial.println(F("):"));

  size_t n = v.size();
  size_t head = std::min<size_t>(n, 5);
  for (size_t i = 0; i < head; ++i) {
    print_person(v[i]);
    Serial.print(i + 1 < head ? F(", ") : F(""));
  }
  if (n > head) {
    Serial.print(F(" ... "));
    size_t tail = std::min<size_t>(5, n);
    for (size_t i = n - tail; i < n; ++i) {
      print_person(v[i]);
      Serial.print(i + 1 < n ? F(", ") : F(""));
    }
  }
  Serial.println();
}

/**
 * @brief Demonstrate VMString usage with STL algorithms.
 */
static void demo_vmstring() {
  Serial.println(F("\n== VMString + STL algorithms =="));

  VMString s("The quick brown fox jumps over the lazy dog");
  Serial.print(F("Initial: ")); Serial.println(s.c_str());

  size_t spaces = std::count(s.begin(), s.end(), ' ');
  Serial.print(F("Spaces: ")); Serial.println(spaces);

  std::replace(s.begin(), s.end(), ' ', '_');
  Serial.print(F("After replace(' ' -> '_'): ")); Serial.println(s.c_str());

  std::transform(s.begin(), s.end(), s.begin(), [](char c) -> char {
    return static_cast<char>(toupper(static_cast<unsigned char>(c)));
  });
  Serial.print(F("Uppercased: ")); Serial.println(s.c_str());

  const char* needle = "BROWN";
  auto it = std::search(s.begin(), s.end(), needle, needle + strlen(needle));
  if (it != s.end()) {
    size_t pos = it.pos();
    Serial.print(F("Found 'BROWN' at pos: ")); Serial.println(pos);
  } else {
    Serial.println(F("'BROWN' not found"));
  }

  s.insert(0, "[TAG] ");
  s.append(" ::END");
  s.replace(0, 6, "[HDR] ");
  Serial.print(F("Mixed ops result: ")); Serial.println(s.c_str());
}

/**
 * @brief Demonstrate VMVector<int> usage with STL algorithms (multi-page).
 */
static void demo_vmvector_int() {
  Serial.println(F("\n== VMVector<int> + STL algorithms =="));

  VMVector<int> v;
  const size_t N = 2048; // ~2 pages if 4KB pages and 4B ints
  v.reserve(N);

  for (size_t i = 0; i < N; ++i) v.push_back(0);

  {
    int start = 0;
    std::for_each(v.begin(), v.end(), [&start](int& x) { x = start++; });
  }
  print_vector_preview(v, "After iota-like fill");

  long long sum = std::accumulate(v.begin(), v.end(), 0LL);
  Serial.print(F("Sum (0..N-1): ")); Serial.println(sum);

  std::transform(v.begin(), v.end(), v.begin(), [](int x) { return x * x; });
  print_vector_preview(v, "After square transform");

  int threshold = 1000;
  size_t cnt = std::count_if(v.begin(), v.end(), [threshold](int x) { return x > threshold; });
  Serial.print(F("Count of elements > ")); Serial.print(threshold); Serial.print(F(": "));
  Serial.println(cnt);

  std::reverse(v.begin(), v.end());
  print_vector_preview(v, "After reverse");

  std::sort(v.begin(), v.end());
  print_vector_preview(v, "After sort ascending");

  int val = 144;
  auto it = std::find(v.begin(), v.end(), val);
  if (it != v.end()) {
    Serial.print(F("Found value ")); Serial.print(val);
    Serial.print(F(" at index ")); Serial.println(it.pos());
  } else {
    Serial.print(F("Value ")); Serial.print(val); Serial.println(F(" not found"));
  }
}

/**
 * @brief Demonstrate VMArray<int, N> usage with STL algorithms.
 */
static void demo_vmarray_int() {
  Serial.println(F("\n== VMArray<int, 32> + STL algorithms =="));

  VMArray<int, 32> a;

  {
    int val = 100;
    std::for_each(a.begin(), a.end(), [&val](int& x) { x = val++; });
  }

  Serial.print(F("Initial: "));
  for (auto it = a.begin(); it != a.end(); ++it) { Serial.print(*it); Serial.print(F(" ")); }
  Serial.println();

  std::rotate(a.begin(), a.begin() + 5, a.end());
  Serial.print(F("After rotate left by 5: "));
  for (auto it = a.begin(); it != a.end(); ++it) { Serial.print(*it); Serial.print(F(" ")); }
  Serial.println();

  std::sort(a.begin(), a.end(), std::greater<int>());
  Serial.print(F("After sort descending: "));
  for (auto it = a.begin(); it != a.end(); ++it) { Serial.print(*it); Serial.print(F(" ")); }
  Serial.println();

  auto itMin = std::min_element(a.begin(), a.end());
  auto itMax = std::max_element(a.begin(), a.end());
  Serial.print(F("Min=")); Serial.print(*itMin);
  Serial.print(F(", Max=")); Serial.println(*itMax);
}

/**
 * @brief Demonstrate VMVector<Person> usage with STL algorithms (non-trivial objects).
 *
 * @details Shows construction via push_back (copy construction), sorting by fields,
 *          transforming via methods, find_if by method, and accumulating by ages.
 */
static void demo_vmvector_objects_nontrivial() {
  Serial.println(F("\n== VMVector<Person> (non-trivial) + STL algorithms =="));

  VMVector<Person> people;

  // Construct objects and push_back (copy semantics)
  people.push_back(Person(30, "alice"));
  people.push_back(Person(22, "bob"));
  people.push_back(Person(27, "carol"));
  people.push_back(Person(35, "dave"));
  people.push_back(Person(29, "erin"));

  print_people_preview(people, "Initial");

  // Sort by age ascending (uses static comparator)
  std::sort(people.begin(), people.end(), Person::by_age_asc);
  print_people_preview(people, "After sort by age");

  // Call a method on each object: uppercase names
  std::for_each(people.begin(), people.end(), [](Person& p){ p.upper_name(); });
  print_people_preview(people, "After uppercasing names");

  // Sort by name ascending (uses method-based comparator)
  std::sort(people.begin(), people.end(), Person::by_name_asc);
  print_people_preview(people, "After sort by name");

  // Find a specific person by name (method + VMString compare)
  auto it = std::find_if(people.begin(), people.end(), [](const Person& p){
    return p.name().compare("DAVE") == 0;
  });
  if (it != people.end()) {
    Serial.print(F("Found 'DAVE' at index ")); Serial.println(it.pos());
  } else {
    Serial.println(F("'DAVE' not found"));
  }

  // Accumulate total age
  long totalAge = std::accumulate(people.begin(), people.end(), 0L, [](long acc, const Person& p){
    return acc + p.age();
  });
  Serial.print(F("Total age: ")); Serial.println(totalAge);
}

/**
 * @brief Demonstrate VMArray<Person, N> with automatic constructors/destructors (no placement new).
 *
 * @details VMArray automatically default-constructs non-trivial elements and destroys them in destructor.
 */
static void demo_vmarray_objects_nontrivial() {
  Serial.println(F("\n== VMArray<Person, 4> (non-trivial, no placement new) =="));

  VMArray<Person, 4> arr;

  // Assign values to already default-constructed elements
  arr[0] = Person(41, "tom");
  arr[1] = Person(18, "amy");
  arr[2] = Person(33, "sam");
  arr[3] = Person(25, "zoe");

  Serial.print(F("Initial: "));
  for (auto it = arr.begin(); it != arr.end(); ++it) { print_person(*it); Serial.print(F(" ")); }
  Serial.println();

  // Invoke methods on elements
  for (auto it = arr.begin(); it != arr.end(); ++it) it->upper_name();

  // Sort by name
  std::sort(arr.begin(), arr.end(), Person::by_name_asc);
  Serial.print(F("After sort by name: "));
  for (auto it = arr.begin(); it != arr.end(); ++it) { print_person(*it); Serial.print(F(" ")); }
  Serial.println();

  // No manual destruction required: VMArray destructor will destroy elements automatically.
}

/**
 * @brief Demonstrate VMPtr<uint32_t> usage (simple primitives) without placement new.
 *
 * @details For VMPtr primitives, use make_vm<T>() to allocate and default-construct storage.
 *          Avoid pointer arithmetic across small-heap blocks; allocate separate pointers if needed.
 */
static void demo_vmptr_primitive() {
  Serial.println(F("\n== VMPtr<uint32_t> (simple usage, no placement new) =="));

  // Allocate and construct via factory (default constructs to 0)
  VMPtr<uint32_t> p  = make_vm<uint32_t>();
  VMPtr<uint32_t> p1 = make_vm<uint32_t>();

  *p  = 42;   // write
  *p1 = 99;   // write

  Serial.print(F("p  value: ")); Serial.println(*p);
  Serial.print(F("p1 value: ")); Serial.println(*p1);

  // If you need a small fixed sequence, prefer VMArray over pointer arithmetic on VMPtr
  VMArray<uint32_t, 8> seq;
  for (size_t i = 0; i < seq.size(); ++i) seq[i] = static_cast<uint32_t>(1000 + i);

  Serial.print(F("Sequence via VMArray[0..7]: "));
  for (size_t i = 0; i < seq.size(); ++i) {
    Serial.print(seq[i]);
    Serial.print(i + 1 < seq.size() ? F(", ") : F(""));
  }
  Serial.println();
}

/**
 * @brief Demonstrate VMPtr<Person> usage with construction via make_vm (no placement new) and methods.
 */
static void demo_vmptr_object_nontrivial() {
  Serial.println(F("\n== VMPtr<Person> (make_vm with ctor + methods, no placement new) =="));

  // Construct with constructor args directly in VM memory via factory
  VMPtr<Person> rp = make_vm<Person>(28, "oscar");

  Serial.print(F("Initial object: ")); print_person(*rp); Serial.println();

  // Call methods
  rp->upper_name();
  rp->age_up(3);

  Serial.print(F("After methods: ")); print_person(*rp); Serial.println();

  // Destroy and reconstruct with new values (no placement new in user code)
  rp.destroy();
  rp = make_vm<Person>(19, "peter");
  Serial.print(F("Reconstructed: ")); print_person(*rp); Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println(F("\n--- MicroSwap containers + STL on ESP32 (SD CS=5) ---"));

  // Initialize SD
  if (!SD.begin(SD_CS_PIN)) {
    fatal("SD.begin() failed. Check wiring and SD card.");
  }
  Serial.println(F("SD card initialized."));

  // Initialize VMManager with SD as backing FS
  if (!VMManager::instance().begin(SD, SWAP_PATH)) {
    fatal("VMManager::begin() failed.");
  }
  Serial.println(F("VMManager initialized with SD swap."));

  // Demos (primitives)
  demo_vmstring();
  demo_vmvector_int();
  demo_vmarray_int();
  demo_vmptr_primitive();

  // Demos (non-trivial objects with constructors and methods)
  demo_vmvector_objects_nontrivial();
  demo_vmarray_objects_nontrivial();
  demo_vmptr_object_nontrivial();

  // Flush pages to swap
  VMManager::instance().flush_all();
  Serial.println(F("\nAll pages flushed. Demo finished."));
}

void loop() {
  // Idle loop
  delay(1000);
}
```

## Mini reference (user-facing API)

Below are user-facing method declarations shown as code-like class definitions. Only differences from standard C++ containers are annotated. Operator overloads are listed in a separate section below.

```cpp
// VM runtime (minimal public surface)
class VMManager {
public:
  static VMManager& instance();

  bool begin(fs::FS& filesystem, const char* swap_path);
  void flush_all();
  void end();

  size_t get_page_size() const;
  size_t get_page_count() const;
};

// VMPtr smart pointer (construct objects with make_vm<T>(...))
template<class T>
class VMPtr {
public:
  VMPtr();                // default, lazy "null-like" state
  bool valid() const;

  // Non-operator accessors (operators listed below)
  T* get();
  const T* get() const;

  int page_index() const;
  size_t page_offset() const;

  void destroy();         // explicitly destroy object and free storage
};

// Factory for VMPtr-managed objects
template<class T, class... Args>
VMPtr<T> make_vm(Args&&... args);

// VMVector — hybrid flat/paged vector
template<class T>
class VMVector {
public:
  using value_type        = T;
  using size_type         = size_t;
  using difference_type   = ptrdiff_t;
  using reference         = value_type&;
  using const_reference   = const value_type&;
  using iterator          = /* random-access iterator; supports it.pos() extension */;
  using const_iterator    = /* random-access iterator; supports it.pos() extension */;
  using reverse_iterator  = /* reverse iterator */;
  using const_reverse_iterator = /* reverse iterator */;

  VMVector();
  VMVector(size_type n, const T& val = T());
  VMVector(std::initializer_list<T> init);
  VMVector(const VMVector& other);
  VMVector(VMVector&& other) noexcept;
  VMVector& operator=(const VMVector& other);
  VMVector& operator=(VMVector&& other) noexcept;
  ~VMVector();

  // Element access (operator[] listed below)
  reference at(size_type idx);
  const_reference at(size_type idx) const;
  reference front();
  const_reference front() const;
  reference back();
  const_reference back() const;

  // Capacity
  bool empty() const;
  size_type size() const;
  size_type capacity() const;
  bool is_flat() const;      // differs from std::vector: true when using a single contiguous small-heap block
  T* data();                 // differs: returns nullptr after transition to paged mode
  const T* data() const;     // differs: returns nullptr after transition to paged mode

  // Modifiers
  void push_back(const T& value);
  template<class... Args> reference emplace_back(Args&&... args);
  template<class... Args> iterator emplace(iterator pos, Args&&... args);
  void pop_back();

  iterator insert(iterator pos, const T& value);
  iterator erase(iterator pos);
  void clear();
  void resize(size_type n, const T& val = T());
  void reserve(size_type n);
  void shrink_to_fit();
  void swap(VMVector& other);

  void assign(size_type n, const T& val);
  template<class InputIt> void assign(InputIt first, InputIt last);

  // Iterators
  iterator begin();
  iterator end();
  const_iterator begin() const;
  const_iterator end() const;
  const_iterator cbegin() const;
  const_iterator cend() const;

  reverse_iterator rbegin();
  reverse_iterator rend();
  const_reverse_iterator rbegin() const;
  const_reverse_iterator rend() const;
  const_reverse_iterator crbegin() const;
  const_reverse_iterator crend() const;
};

// VMArray — fixed-size array on small heap
template<class T, size_t N>
class VMArray {
public:
  using value_type        = T;
  using size_type         = size_t;
  using difference_type   = ptrdiff_t;
  using reference         = value_type&;
  using const_reference   = const value_type&;
  using iterator          = /* random-access iterator; supports it.pos() extension */;
  using const_iterator    = /* random-access iterator; supports it.pos() extension */;
  using reverse_iterator  = /* reverse iterator */;
  using const_reverse_iterator = /* reverse iterator */;

  VMArray();    // trivial T -> zero-initialized memory; non-trivial T -> default-constructed elements
  ~VMArray();   // non-trivial T -> calls destructors; always frees storage

  // Element access (operator[] listed below)
  reference at(size_type idx);
  const_reference at(size_type idx) const;

  // Capacity
  constexpr size_type size() const;   // returns N
  constexpr bool empty() const;       // returns N == 0

  // Modifiers
  void fill(const T& val);
  void clear();      // differs from std::array: assigns T() to each element and flushes page

  // Iterators
  iterator begin();
  iterator end();
  const_iterator begin() const;
  const_iterator end() const;
  const_iterator cbegin() const;
  const_iterator cend() const;

  reverse_iterator rbegin();
  reverse_iterator rend();
  const_reverse_iterator rbegin() const;
  const_reverse_iterator rend() const;
  const_reverse_iterator crbegin() const;
  const_reverse_iterator crend() const;
};

// VMString — single-block mutable string on small heap
class VMString {
public:
  using value_type        = char;
  using size_type         = size_t;
  using difference_type   = ptrdiff_t;
  using reference         = value_type&;
  using const_reference   = const value_type&;
  using iterator          = /* random-access iterator; supports it.pos() extension */;
  using const_iterator    = /* random-access iterator; supports it.pos() extension */;
  using reverse_iterator  = /* reverse iterator */;
  using const_reverse_iterator = /* reverse iterator */;

  static constexpr size_type npos = static_cast<size_type>(-1);

  // Constructors / assignment
  explicit VMString(size_type initial_capacity = 64);   // allocates a single heap block
  VMString(const char* s);
  VMString(const char* s, size_type count);
  VMString(size_type count, char ch);
  VMString(const VMString& other);
  VMString(VMString&& other) noexcept;
  VMString& operator=(const VMString& other);
  VMString& operator=(VMString&& other) noexcept;
  ~VMString();

  // Iterators
  iterator begin();
  iterator end();
  const_iterator begin() const;
  const_iterator end() const;
  const_iterator cbegin() const;
  const_iterator cend() const;
  reverse_iterator rbegin();
  reverse_iterator rend();
  const_reverse_iterator rbegin() const;
  const_reverse_iterator rend() const;
  const_reverse_iterator crbegin() const;
  const_reverse_iterator crend() const;

  // Element access (operator[] listed below)
  reference at(size_type pos);
  const_reference at(size_type pos) const;
  reference front();
  const_reference front() const;
  reference back();
  const_reference back() const;

  const char* c_str() const;

  // Capacity
  bool empty() const;
  size_type size() const;
  size_type length() const;
  size_type capacity() const;
  size_type max_size() const;   // differs: theoretical single-block limit
  void reserve(size_type new_cap);  // differs: throws if exceeding one-block capacity
  void shrink_to_fit();              // differs: no-op (single block)
  void resize(size_type new_size, char ch = '\0');

  // Assign / append
  void assign(const char* s);
  void assign(const char* s, size_type count);
  void assign(size_type count, char ch);
  void assign(const VMString& other, size_type pos, size_type count = npos);

  VMString& append(const char* s);
  VMString& append(const char* s, size_type count);
  VMString& append(const VMString& other);
  VMString& append(size_type count, char ch);

  void push_back(char c);
  void pop_back();

  // Insert / erase / replace / substr
  VMString& insert(size_type pos, const char* s);
  VMString& insert(size_type pos, const char* s, size_type count);
  VMString& insert(size_type pos, size_type count, char ch);

  VMString& erase(size_type pos = 0, size_type count = npos);

  VMString& replace(size_type pos, size_type count, const char* s);
  VMString& replace(size_type pos, size_type count, const VMString& other);
  VMString substr(size_type pos = 0, size_type count = npos) const;

  // Copy / swap
  size_type copy(char* dest, size_type count, size_type pos = 0) const;
  void swap(VMString& other);

  // Find family
  size_type find(const char* s, size_type pos, size_type count) const;
  size_type find(const char* s, size_type pos = 0) const;
  size_type find(const VMString& other, size_type pos = 0) const;
  size_type find(char ch, size_type pos = 0) const;

  size_type rfind(const char* s, size_type pos, size_type count) const;
  size_type rfind(const char* s, size_type pos = npos) const;
  size_type rfind(const VMString& other, size_type pos = npos) const;
  size_type rfind(char ch, size_type pos = npos) const;

  size_type find_first_of(const char* s, size_type pos = 0) const;
  size_type find_first_of(const VMString& other, size_type pos = 0) const;
  size_type find_first_of(char ch, size_type pos = 0) const;

  size_type find_last_of(const char* s, size_type pos = npos) const;
  size_type find_last_of(const VMString& other, size_type pos = npos) const;
  size_type find_last_of(char ch, size_type pos = npos) const;

  size_type find_first_not_of(const char* s, size_type pos = 0) const;
  size_type find_first_not_of(const VMString& other, size_type pos = 0) const;
  size_type find_first_not_of(char ch, size_type pos = 0) const;

  size_type find_last_not_of(const char* s, size_type pos = npos) const;
  size_type find_last_not_of(const VMString& other, size_type pos = npos) const;
  size_type find_last_not_of(char ch, size_type pos = npos) const;

  int compare(const VMString& other) const;
  int compare(const char* s) const;

  void clear();  // differs: flushes and resets internal buffer pointer to avoid dangling after swap
};
```

### Operator overloads
- VMPtr<T>:
  - Dereference and member access: operator*(), operator->()
  - Comparisons: ==, !=, <, >, <=, >=
  - Pointer arithmetic and indexing: operator+(ptrdiff_t), operator-(ptrdiff_t), difference (ptrdiff_t operator-(const VMPtr&)), ++/-- (pre/post), +=, -=, operator[](ptrdiff_t)
- VMVector<T>:
  - Comparisons: ==, !=, <, >, <=, >=
  - Element access: operator[](size_type) (const and non-const)
- VMArray<T, N>:
  - Element access: operator[](size_type) (const and non-const)
- VMString:
  - Element access: operator[](size_type) (const and non-const)
  - Concatenation and append: operator+= (char, const char*, VMString)
  - Concatenation: operator+(VMString, VMString), operator+(VMString, const char*), operator+(const char*, VMString)
  - Comparisons: ==, !=, <, >, <=, >=

## Notes and limitations
- VMVector hybrid storage: starts flat and may transition to paged storage; after transition, data() returns nullptr and contiguous access is not available.
- VMString is limited to a single small-heap block; reserve/resize beyond one block throws.
- Small-heap payload alignment is 8 bytes; types requiring stricter alignment may not be supported on all targets.
- Not thread-safe.

Happy swapping!
