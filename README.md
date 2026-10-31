# MicroSwap

Virtual memory with a swap file, and STL-like containers on top of it, for Arduino boards with little RAM.

MicroSwap keeps a few pages of data in RAM and stores the rest in a file on an SD card or flash. Pages are loaded back automatically when you access them, and the least recently used page is written out to make room. An Arduino Uno can hold a `VMVector<long>` of thousands of elements while using about 0.7 KB of RAM.

- `VMVector<T>`: vector, works with `std::sort` and other algorithms where the STL exists
- `VMArray<T, N>`: fixed-size array
- `VMString`: mutable string
- `VMPtr<T>` + `make_vm<T>(...)`: one object in virtual memory
- `VMPin<T>`: keeps an object's page in RAM while you hold a reference to it

## Supported boards

Any Arduino core with a C++11 compiler. The library uses neither exceptions nor RTTI, and needs no STL or `FS.h`. Every push is compiled in CI for:

| Board | Core | `<algorithm>` support |
|---|---|---|
| Arduino Uno, Mega | AVR | no (the core has no STL) |
| Arduino MKR Zero | SAMD | yes |
| Arduino Uno R4 Minima | Renesas | yes |
| ESP32, ESP32-C3 | esp32 | yes |
| NodeMCU (ESP8266) | esp8266 | yes |
| Blue Pill (STM32F103) | STM32duino | yes |
| Raspberry Pi Pico | arduino-pico | yes |

## Installation

**Arduino IDE.** Download the repository as a ZIP (*Code → Download ZIP* on GitHub) and use *Sketch → Include Library → Add .ZIP Library*. The examples also need the `SD` library, which comes with the IDE; ESP and RP2040 cores have their own.

**PlatformIO.** Add the library to `platformio.ini`:

```ini
[env:uno]
platform = atmelavr
board = uno
framework = arduino
lib_deps =
    https://github.com/espmaniac/MicroSwap.git
    arduino-libraries/SD    ; for the examples; not needed on ESP32 / ESP8266 / RP2040
```

To change the configuration, use build flags so that every file sees the same values:

```ini
build_flags = -DVM_RESIDENT_PAGES=4
```

**Arduino CLI.** `arduino-cli lib install --git-url https://github.com/espmaniac/MicroSwap.git` (needs `library.enable_unsafe_install` set to `true`).

## Examples

| Example | Shows |
|---|---|
| [Basic](examples/Basic/Basic.ino) | A vector bigger than RAM, a string, swap statistics |
| [Vector](examples/Vector/Vector.ino) | Every `VMVector` operation: insert, erase, iterators, flat and paged storage, resize, copy, swap, compare |
| [Array](examples/Array/Array.ino) | `VMArray` as a global, arrays of structs, fill, copy, iterators |
| [String](examples/String/String.ino) | Building, searching, extracting, editing and comparing `VMString` |
| [Pointer](examples/Pointer/Pointer.ino) | `make_vm`, `VMPtr` as a long-lived object used from `loop()`, `destroy()` |
| [Pin](examples/Pin/Pin.ino) | Keeping references and C strings valid with `VMPin`, and the pin limit |
| [ErrorHandling](examples/ErrorHandling/ErrorHandling.ino) | A custom error handler and every kind of error |
| [Algorithms](examples/Algorithms/Algorithms.ino) | `std::sort`, `std::accumulate`, `std::transform` on containers (not on AVR) |
| [CustomStorage](examples/CustomStorage/CustomStorage.ino) | Swapping to an SPI SRAM chip by implementing `VMStorage` |

All examples except CustomStorage put the swap file on an SD card, through `SwapFile.h` in the example's folder. Set `SD_CS_PIN` there to your card's chip select pin.

## Quick start

```cpp
#include <SPI.h>
#include <SD.h>
#include <MicroSwap.h>

File swapFile;
VMFileStorage<File> swapStorage(swapFile);

void setup() {
    Serial.begin(115200);
    SD.begin(SS);

    // The swap file must be open for reading and writing (see below).
    SD.remove("/swap.bin");
    swapFile = SD.open("/swap.bin", O_READ | O_WRITE | O_CREAT);
    VMManager::instance().begin(swapStorage);

    VMVector<long> v;
    for (long i = 0; i < 1000; ++i) v.push_back(i * i);
    Serial.println(v[999]); // 998001
}

void loop() {}
```

`SwapFile.h` in [`examples/Basic`](examples/Basic) opens the swap file the right way on each board; see [Examples](#examples) for the rest.

### Opening the swap file

The file must be opened for both reading and writing and must stay open:

| Library | Open with |
|---|---|
| ESP32 `SD` | `SD.open(path, "w+")` |
| ESP8266 / RP2040 `SDFS` | `SDFS.open(path, "w+")` |
| Arduino `SD` (AVR, SAMD, STM32, ...) | `SD.open(path, O_READ \| O_WRITE \| O_CREAT)` |
| SdFat | `sd.open(path, O_RDWR \| O_CREAT \| O_TRUNC)` |

Do not use `FILE_WRITE` with the Arduino `SD` library: it appends every write to the end of the file.

Prefer an SD card over the board's internal flash (`LittleFS`, `SPIFFS`). Swapping rewrites the same pages over and over, and internal flash wears out after a limited number of writes. If you must use it, raise `VM_RESIDENT_PAGES` to swap less.

To store pages somewhere else (external SPI RAM, FRAM, ...), implement `VMStorage`:

```cpp
class MyStorage : public VMStorage {
public:
    bool read(uint32_t offset, uint8_t* buf, size_t len) override { /* ... */ }
    bool write(uint32_t offset, const uint8_t* buf, size_t len) override { /* ... */ }
};
```

## Configuration

Define these as build flags, or with `#define` before `#include <MicroSwap.h>` in every file that includes it:

| Macro | Meaning | AVR | ESP32 | Others |
|---|---|---|---|---|
| `VM_PAGE_SIZE` | Bytes per page; also the largest string, array or object | 128 | 4096 | 1024 |
| `VM_RESIDENT_PAGES` | Pages kept in RAM (4 to 254) | 4 | 8 | 5 |
| `VM_PAGE_COUNT` | Total virtual pages | 48 | 256 | 128 |
| `VM_HEAP_ALIGN` | Alignment of stored objects | 2 | 8 | 8 |

RAM used is about `VM_PAGE_SIZE * VM_RESIDENT_PAGES + 4 * VM_PAGE_COUNT` bytes. The swap file grows up to `VM_PAGE_SIZE * VM_PAGE_COUNT` bytes. More resident pages mean fewer swaps; smaller pages mean less data written per swap.

A page is written back only if it changed. Pages read through non-const references (`v[i]` on a non-const vector, iterators passed to algorithms) are compared with their stored copy before eviction and are not rewritten when nothing changed.

## Errors

Errors (out of pages, storage failure, index out of range, ...) are passed to an error handler. The default handler calls `abort()`. Set your own to log and reset, or to return:

```cpp
void onVMError(VMError error) {
    Serial.print(F("MicroSwap error: "));
    Serial.println(vm_error_name(error));
}

VMManager::instance().set_error_handler(onVMError);
```

If the handler returns, the failed operation does nothing; for example, `push_back` does not add the element. Element access then returns a placeholder object with meaningless contents. `VMManager::instance().last_error()` returns the last error.

## Rules to follow

**Use references right away.** `v[i]`, `*ptr`, `s.c_str()` and iterators' `*it` return references into a page in RAM. If other pages are loaded afterwards, that page may be swapped out and the reference then points to other data. At least `VM_RESIDENT_PAGES - 1` other pages can be touched safely. That is enough for the containers' own operations. `std::sort` over vectors of numbers and of objects with `VMString` members is covered by the tests, but other algorithms, or comparators that touch many objects at once, can exceed the limit; raise `VM_RESIDENT_PAGES` if in doubt. Do not keep a reference while doing a lot of other work:

```cpp
long& first = v[0];
for (size_t i = 0; i < v.size(); ++i) total += v[i]; // may evict v[0]'s page
first = 5;                                         // wrong: `first` may be stale

v[0] = 5;                                          // right
```

**Pin what you keep.** To keep a reference for longer, hold a `VMPin`. While it lives, the page of the object stays in RAM and the reference stays valid:

```cpp
VMPin<long> first(v[0]);                           // or: auto first = vm_pin(v[0]);
for (size_t i = 0; i < v.size(); ++i) total += v[i];
*first = 5;                                        // right: the page was kept in RAM
```

The containers need 4 unpinned pages for their own work, so at most `VM_RESIDENT_PAGES - 4` different pages can be pinned at once; pins on the same page count once. That is one pin by default, four on ESP32, and none on AVR: there, set `VM_RESIDENT_PAGES` to 5 to pin one page (or 5 pages of 64 bytes, as the `Pin` example does, to keep the RAM use). A pin that does not fit reports `VMError::TooManyPins` and `valid()` returns false. A pin keeps a page, not an element: do not `push_back`, `insert`, `erase` or destroy the container while a pin into it is alive.

**Containers inside containers.** A container can be stored inside virtual memory, such as a `VMString` member of a vector element or a vector of vectors. Each operation keeps in RAM the page of the container itself and of the objects passed to it, so this works like any other use. It needs up to 4 resident pages at once, plus one for each further level of nesting (a vector of vectors of objects with strings needs 5). If there are not enough, the operation reports `VMError::TooManyPins` before anything is changed; the default handler stops the program there. A handler that returns lets the operation continue, and data may then be corrupted.

**Limits.** A `VMString`, `VMArray` or `VMPtr` object must fit in one page (`VMString::max_size()` is `VM_PAGE_SIZE - 5` on AVR). `VMVector` elements must fit in one page each; the vector itself can span all pages.

**Not thread-safe.** Use it from one task only, or protect it with a mutex.

**Data does not survive a reset.** `begin()` starts with empty memory.

## API reference

Every public member, written as class declarations. Only behavior that differs from the standard C++ library is commented. All containers allocate memory on first use, so they can be global variables created before `VMManager::begin()`.

### Errors

```cpp
enum class VMError : uint8_t {
    None,
    NotStarted,   // VMManager::begin() has not been called
    OutOfPages,   // every virtual page is in use
    TooLarge,     // the object does not fit in one page
    OutOfMemory,  // malloc failed (VMVector chunk table)
    StorageIO,    // reading or writing the swap file failed
    OutOfRange,   // index out of range: at(), front()/back()/pop_back() on empty, ...
    NullPointer,  // a null VMPtr was dereferenced
    TooManyPins,  // a VMPin would leave fewer than 4 resident pages unpinned, or every page is pinned
};

typedef void (*VMErrorHandler)(VMError error);

const char* vm_error_name(VMError error);   // "OutOfPages", ...
void vm_default_error_handler(VMError);     // calls abort()
```

### Swap storage

```cpp
// Interface: implement it to store pages anywhere.
class VMStorage {
public:
    virtual ~VMStorage();
    virtual bool read(uint32_t offset, uint8_t* buf, size_t len) = 0;
    virtual bool write(uint32_t offset, const uint8_t* buf, size_t len) = 0;
    virtual bool flush();   // called by VMManager::flush_all(); default does nothing
};

// Storage on any Arduino File with seek(), read(), write() and flush().
// The file must be open for reading and writing and stay open.
template<class FileT>
class VMFileStorage : public VMStorage {
public:
    explicit VMFileStorage(FileT& file);
    bool read(uint32_t offset, uint8_t* buf, size_t len) override;
    bool write(uint32_t offset, const uint8_t* buf, size_t len) override;
    bool flush() override;   // file.flush()
};
```

### VMManager

```cpp
class VMManager {
public:
    static VMManager& instance();

    bool begin(VMStorage& storage);   // discards all previous data
    void end();                       // discards all pages, writes nothing
    bool flush_all();                 // writes modified resident pages, then storage.flush(); false if a write failed

    VMError last_error() const;       // last error since begin() or clear_error()
    void clear_error();
    void set_error_handler(VMErrorHandler handler);   // nullptr restores the default (abort())

    size_t get_page_size() const;       // VM_PAGE_SIZE
    size_t get_page_count() const;      // VM_PAGE_COUNT
    size_t get_resident_pages() const;  // VM_RESIDENT_PAGES
    size_t free_pages() const;          // virtual pages not in use
    uint32_t swap_in_count() const;     // pages loaded from storage since begin()
    uint32_t swap_out_count() const;    // pages written to storage since begin()
};
```

### VMPtr

```cpp
// Pointer to one object in virtual memory. Copies point to the same object,
// like a raw pointer; the object lives until destroy().
template<class T>
class VMPtr {
public:
    VMPtr();                          // null
    bool valid() const;               // false if null
    explicit operator bool() const;

    T& operator*();                   // null -> VMError::NullPointer
    const T& operator*() const;
    T* operator->();
    const T* operator->() const;
    T* get();                         // pointer into a resident page, see "Rules to follow"
    const T* get() const;

    void destroy();                   // runs ~T(), frees storage, becomes null

    bool operator==(const VMPtr& other) const;
    bool operator!=(const VMPtr& other) const;
};

// Like std::make_unique. Returns a null VMPtr if allocation fails.
template<class T, class... Args>
VMPtr<T> make_vm(Args&&... args);
```

### VMPin

```cpp
// Keeps the page holding an object in RAM while the pin lives (see "Rules to follow").
// Movable, not copyable.
template<class T>
class VMPin {
public:
    explicit VMPin(T& object);    // pass a reference just obtained from a container: VMPin<long> p(v[0]);
    explicit VMPin(T* object);    // e.g. VMPin<const char> text(s.c_str());
    VMPin(VMPin&& other) noexcept;
    ~VMPin();                     // unpins

    bool valid() const;           // false if not pinned (TooManyPins, or not in virtual memory)
    explicit operator bool() const;

    T& operator*() const;
    T* operator->() const;
    T* get() const;

    void release();               // unpin early
};

template<class T> VMPin<T> vm_pin(T& object);   // auto p = vm_pin(v[0]);
template<class T> VMPin<T> vm_pin(T* object);   // auto text = vm_pin(s.c_str());
```

### VMVector

```cpp
template<class T>
class VMVector {
public:
    typedef T         value_type;
    typedef T&        reference;
    typedef const T&  const_reference;
    typedef size_t    size_type;
    typedef ptrdiff_t difference_type;
    using iterator               = /* random-access; it.pos() returns the index */;
    using const_iterator         = /* random-access; it.pos() returns the index */;
    using reverse_iterator       = /* ... */;
    using const_reverse_iterator = /* ... */;

    VMVector();
    explicit VMVector(size_type n, const T& value = T());
    VMVector(std::initializer_list<T> init);   // only where the STL exists (not AVR)
    VMVector(const VMVector& other);
    VMVector(VMVector&& other) noexcept;
    VMVector& operator=(const VMVector& other);
    VMVector& operator=(VMVector&& other) noexcept;
    ~VMVector();

    // Element access
    reference       operator[](size_type i);
    const_reference operator[](size_type i) const;
    reference       at(size_type i);           // differs: out of range -> VMError::OutOfRange, no exception
    const_reference at(size_type i) const;
    reference       front();                   // differs: checked, empty -> VMError::OutOfRange
    const_reference front() const;
    reference       back();                    // differs: checked, empty -> VMError::OutOfRange
    const_reference back() const;
    T*       data();                           // differs: nullptr unless is_flat()
    const T* data() const;

    // Capacity
    bool      empty() const;
    size_type size() const;
    size_type capacity() const;
    bool      is_flat() const;                 // differs: true while all elements are in one heap block
    void      reserve(size_type n);
    void      shrink_to_fit();                 // differs: releases unused pages; no-op while is_flat()

    // Modifiers (on failure the error handler is called and nothing changes)
    void push_back(const T& value);
    void push_back(T&& value);
    template<class... Args> reference emplace_back(Args&&... args);   // differs: placeholder on failure
    template<class... Args> iterator emplace(const_iterator pos, Args&&... args);
    iterator insert(const_iterator pos, const T& value);
    iterator insert(const_iterator pos, T&& value);
    iterator erase(const_iterator pos);
    void pop_back();                           // differs: empty -> VMError::OutOfRange
    void clear();                              // differs: also frees all storage
    void resize(size_type n, const T& value = T());
    void assign(size_type n, const T& value);
    template<class InputIt> void assign(InputIt first, InputIt last);
    void swap(VMVector& other);

    // Iterators
    iterator               begin();
    iterator               end();
    const_iterator         begin() const;
    const_iterator         end() const;
    const_iterator         cbegin() const;
    const_iterator         cend() const;
    reverse_iterator       rbegin();
    reverse_iterator       rend();
    const_reverse_iterator rbegin() const;
    const_reverse_iterator rend() const;
    const_reverse_iterator crbegin() const;
    const_reverse_iterator crend() const;

    // Comparisons
    bool operator==(const VMVector& other) const;
    bool operator!=(const VMVector& other) const;
    bool operator<(const VMVector& other) const;
    bool operator>(const VMVector& other) const;
    bool operator<=(const VMVector& other) const;
    bool operator>=(const VMVector& other) const;
};
```

### VMArray

```cpp
// N * sizeof(T) must fit in one page (checked at compile time).
template<class T, size_t N>
class VMArray {
public:
    typedef T         value_type;
    typedef T&        reference;
    typedef const T&  const_reference;
    typedef size_t    size_type;
    typedef ptrdiff_t difference_type;
    using iterator               = /* random-access; it.pos() returns the index */;
    using const_iterator         = /* random-access; it.pos() returns the index */;
    using reverse_iterator       = /* ... */;
    using const_reverse_iterator = /* ... */;

    VMArray();                                 // elements value-initialized (0 for int) on first access
    VMArray(const VMArray& other);
    VMArray(VMArray&& other) noexcept;
    VMArray& operator=(const VMArray& other);
    VMArray& operator=(VMArray&& other) noexcept;
    ~VMArray();

    reference       operator[](size_type i);
    const_reference operator[](size_type i) const;
    reference       at(size_type i);           // differs: out of range -> VMError::OutOfRange, no exception
    const_reference at(size_type i) const;
    reference       front();
    const_reference front() const;
    reference       back();
    const_reference back() const;

    constexpr size_type size() const;          // N
    constexpr bool      empty() const;         // N == 0

    void fill(const T& value);
    void clear();                              // differs: not in std::array; assigns T() to every element

    iterator               begin();
    iterator               end();
    const_iterator         begin() const;
    const_iterator         end() const;
    const_iterator         cbegin() const;
    const_iterator         cend() const;
    reverse_iterator       rbegin();
    reverse_iterator       rend();
    const_reverse_iterator rbegin() const;
    const_reverse_iterator rend() const;
    const_reverse_iterator crbegin() const;
    const_reverse_iterator crend() const;
};
```

### VMString

```cpp
// Stored in one heap block: at most max_size() characters. An empty string uses no memory.
class VMString {
public:
    typedef char      value_type;
    typedef char&     reference;
    typedef const char& const_reference;
    typedef size_t    size_type;
    typedef ptrdiff_t difference_type;
    using iterator               = /* random-access; it.pos() returns the index */;
    using const_iterator         = /* random-access; it.pos() returns the index */;
    using reverse_iterator       = /* ... */;
    using const_reverse_iterator = /* ... */;

    static const size_type npos = size_type(-1);

    // Construction / assignment
    VMString();
    explicit VMString(size_type initial_capacity);   // differs: reserves capacity
    VMString(const char* s);
    VMString(const char* s, size_type count);
    VMString(size_type count, char ch);
    VMString(const VMString& other);
    VMString(VMString&& other) noexcept;
    VMString& operator=(const VMString& other);
    VMString& operator=(VMString&& other) noexcept;
    VMString& operator=(const char* s);
    ~VMString();

    // Iterators
    iterator               begin();
    iterator               end();
    const_iterator         begin() const;
    const_iterator         end() const;
    const_iterator         cbegin() const;
    const_iterator         cend() const;
    reverse_iterator       rbegin();
    reverse_iterator       rend();
    const_reverse_iterator rbegin() const;
    const_reverse_iterator rend() const;
    const_reverse_iterator crbegin() const;
    const_reverse_iterator crend() const;

    // Element access (differs: every accessor is checked; out of range -> VMError::OutOfRange)
    reference       operator[](size_type i);
    const_reference operator[](size_type i) const;
    reference       at(size_type i);
    const_reference at(size_type i) const;
    reference       front();
    const_reference front() const;
    reference       back();
    const_reference back() const;
    const char*     c_str() const;             // pointer into a resident page, see "Rules to follow"

    // Capacity
    bool      empty() const;
    size_type size() const;
    size_type length() const;
    size_type capacity() const;
    static constexpr size_type max_size();     // differs: one page minus a few bytes (123 on AVR)
    void reserve(size_type new_cap);           // differs: beyond max_size() -> VMError::TooLarge
    void shrink_to_fit();                      // differs: no-op
    void resize(size_type n, char ch = '\0');
    void clear();

    // Modifiers (growing beyond max_size() -> VMError::TooLarge, string unchanged)
    void assign(const char* s);                // differs: returns void
    void assign(const char* s, size_type count);
    void assign(size_type count, char ch);
    void assign(const VMString& other, size_type pos, size_type count = npos);

    VMString& append(const char* s);
    VMString& append(const char* s, size_type count);
    VMString& append(const VMString& other);
    VMString& append(size_type count, char ch);
    void push_back(char c);
    void pop_back();                           // differs: empty -> VMError::OutOfRange

    VMString& operator+=(const char* s);
    VMString& operator+=(const VMString& s);
    VMString& operator+=(char c);

    VMString& insert(size_type pos, const char* s);
    VMString& insert(size_type pos, const char* s, size_type count);
    VMString& insert(size_type pos, const VMString& other);
    VMString& insert(size_type pos, size_type count, char ch);

    VMString& erase(size_type pos = 0, size_type count = npos);

    VMString& replace(size_type pos, size_type count, const char* s, size_type s_count);
    VMString& replace(size_type pos, size_type count, const char* s);
    VMString& replace(size_type pos, size_type count, const VMString& other);

    VMString  substr(size_type pos = 0, size_type count = npos) const;
    size_type copy(char* dest, size_type count, size_type pos = 0) const;
    void      swap(VMString& other);

    // Search
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

    size_type find_first_not_of(const char* s, size_type pos = 0) const;
    size_type find_first_not_of(const VMString& other, size_type pos = 0) const;
    size_type find_first_not_of(char ch, size_type pos = 0) const;

    size_type find_last_of(const char* s, size_type pos = npos) const;
    size_type find_last_of(const VMString& other, size_type pos = npos) const;
    size_type find_last_of(char ch, size_type pos = npos) const;

    size_type find_last_not_of(const char* s, size_type pos = npos) const;
    size_type find_last_not_of(const VMString& other, size_type pos = npos) const;
    size_type find_last_not_of(char ch, size_type pos = npos) const;

    // Comparison
    int compare(const char* s, size_type s_len) const;
    int compare(const char* s) const;
    int compare(const VMString& other) const;

    bool operator==(const VMString& other) const;
    bool operator!=(const VMString& other) const;
    bool operator<(const VMString& other) const;
    bool operator>(const VMString& other) const;
    bool operator<=(const VMString& other) const;
    bool operator>=(const VMString& other) const;
    bool operator==(const char* s) const;
    bool operator!=(const char* s) const;
};

VMString operator+(const VMString& a, const VMString& b);
VMString operator+(const VMString& a, const char* b);
VMString operator+(const char* a, const VMString& b);
```

## Running tests

The library can be tested on a desktop machine without any board:

```sh
make -C tests
```

The tests are built twice, with sanitizers: once with the default configuration, and once in an AVR-like configuration (C++11, no STL, 128-byte pages, 3 resident pages) where pages are swapped constantly. GitHub Actions runs them and compiles the examples for every board listed above.

## Upgrading from earlier versions

- Include `<MicroSwap.h>` instead of `containers.h`.
- `VMManager::begin(fs, path)` became `begin(VMStorage&)`: open the file yourself and wrap it in `VMFileStorage<File>`.
- Errors no longer throw exceptions; see [Errors](#errors).
- `VMPtr` no longer supports pointer arithmetic, and a default-constructed `VMPtr` is null instead of allocating on first use.
- `VM_PAGE_COUNT` now counts virtual pages; only `VM_RESIDENT_PAGES` of them are in RAM.

## License

Public domain ([The Unlicense](LICENSE)).
