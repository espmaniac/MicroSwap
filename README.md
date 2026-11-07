# MicroSwap

Virtual memory with a swap file, and STL-like containers on top of it, for Arduino boards with little RAM.

MicroSwap keeps a few pages of data in RAM and stores the rest in a file on an SD card or flash. Pages are loaded back automatically when you access them, and the least recently used page is written out to make room. An Arduino Uno can hold a `VMVector<long>` of thousands of elements while using about 0.7 KB of RAM.

- `VMVector<T>`: vector, works with `std::sort` and other algorithms where the STL exists
- `VMRingBuffer<T>`: ring buffer of the last N elements (sensor readings, logs), or a queue
- `VMArray<T, N>`: fixed-size array
- `VMString`: mutable string, of any length (a long one is stored in pages, like a vector)
- `VMPtr<T>` + `make_vm<T>(...)`: one object in virtual memory
- `VMPin<T>`: keeps an object's page in RAM while you hold a reference to it

## Supported boards

Any Arduino core with a C++11 compiler. The library uses no exceptions, no RTTI and no `malloc` (all of its RAM is static, so the IDE reports it at compile time), and needs no STL or `FS.h`. Every push is compiled in CI for:

| Board | Core | `<algorithm>` support |
|---|---|---|
| Arduino Uno, Mega | AVR | no (the core has no STL) |
| Arduino Nano Every | megaAVR | no |
| Arduino Due | SAM | no |
| Arduino Nano 33 BLE | Mbed (nRF52840) | yes |
| Arduino Nano RP2040 Connect | Mbed (RP2040) | yes |
| Teensy 4.1 | Teensyduino | yes |
| Arduino MKR Zero | SAMD | yes |
| Arduino Uno R4 Minima | Renesas | yes |
| ESP32, ESP32-C3, ESP32-S3 | esp32 | yes |
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
| [RingBuffer](examples/RingBuffer/RingBuffer.ino) | The last 2000 sensor readings in a `VMRingBuffer`: average, minimum, maximum, and use as a queue |
| [SpiMemory](examples/SpiMemory/SpiMemory.ino) | Swapping to an SPI memory chip (23LC1024 SRAM, PSRAM, FRAM) with `VMSpiMemoryStorage` |
| [CustomStorage](examples/CustomStorage/CustomStorage.ino) | A storage of your own on two functions, with `VMCustomStorage` |

All examples except SpiMemory and CustomStorage put the swap file on an SD card, through `SwapFile.h` in the example's folder. Set `SD_CS_PIN` there to your card's chip select pin.

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

On an ESP32 the first `SD.begin()` after a reset can fail: the reset does not cut the card's power, and the card may still be in the middle of a command. Calling it again works, so the examples try up to three times. Many SD modules (HW-125 and alike) need 5 V on VCC, as they have their own 3.3 V regulator, and do not release MISO when the card is not selected: give such a module an SPI bus of its own. With long wires, keep the SPI clock at 10 MHz or less.

Prefer an SD card over the board's internal flash (`LittleFS`, `SPIFFS`). Swapping rewrites the same pages over and over, and internal flash wears out after a limited number of writes. If you must use it, raise `VM_RESIDENT_PAGES` to swap less.

### SPI memory chips instead of a file

An SPI memory chip is a good place for swapped pages: it needs no file system and no SD library, it is faster than a card, and SRAM, PSRAM, FRAM and MRAM do not wear out. `MicroSwapSpiMemory.h` has a storage for them; pick your chip from `VMSpiChips`:

```cpp
#include <MicroSwapSpiMemory.h>

VMSpiMemoryStorage memory(10, VMSpiChips::LC1024);   // chip select pin, chip; then SPI clock (8 MHz), SPIClass

void setup() {
    if (!memory.begin()) { /* no chip, smaller than described, or the virtual memory does not fit */ }
    VMManager::instance().begin(memory);
}
```

| `VMSpiChips::` | Chip | Size |
|---|---|---|
| `LC1024` | Microchip 23LC1024 / 23A1024 SRAM | 128 KB |
| `LC512` | Microchip 23LC512 / 23A512 SRAM | 64 KB |
| `K256`, `K640` | Microchip 23K256, 23K640 SRAM | 32 KB, 8 KB |
| `APS6404` | APS6404L / ESP-PSRAM64H / LY68L6400 PSRAM (3.3 V) | 8 MB |
| `MB85RS64V` | Fujitsu MB85RS64V FRAM | 8 KB |
| `FM25V10` | Infineon FM25V10 FRAM | 128 KB |

The descriptions come from the datasheets; so far the storage is tested against simulated chips only (see "Tested on hardware").

**A chip that is not in the list.** Describe it: chips differ only in a few numbers, so no code is needed for one that reads and writes by address like these:

```cpp
// capacity, address bytes, max clock, flags, max chip-select time (us), page bytes,
// read command, write command, dummy bytes after a read's address
const VMSpiChip myChip(65536UL, 2, 20000000UL, kVMSpiWriteEnable);
VMSpiMemoryStorage memory(10, myChip);
```

Flags: `kVMSpiModeRegister` (set sequential mode at start, Microchip SRAM), `kVMSpiWriteEnable` (write enable before every write, FRAM and MRAM), `kVMSpiReset` (reset at start, PSRAM). PSRAM also needs its chip-select time (8 µs) and page size (1 KB): transfers are split to respect both, so a faster SPI clock makes it faster.

`begin()` checks that the description is valid, that the chip answers, that it really holds the described capacity (a smaller chip mirrors its first half) and that the virtual memory (`VM_PAGE_SIZE * VM_PAGE_COUNT`) fits. The chip shares the SPI bus with other devices: each transfer takes the bus with its own settings and chip select. Flash and EEPROM chips are not for swapping: they need erasing or slow page writes, and they wear out.

### Storage of your own

For any other memory, give MicroSwap two functions with `VMCustomStorage`:

```cpp
bool chipRead(void* context, uint32_t offset, uint8_t* buf, size_t len)        { /* ... */ return true; }
bool chipWrite(void* context, uint32_t offset, const uint8_t* buf, size_t len) { /* ... */ return true; }

VMCustomStorage storage(chipRead, chipWrite);   // optional: context passed to both, flush function
```

or implement `VMStorage`:

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
| `VM_TICK_BITS` | Width of the least-recently-used counter (16 or 32) | 16 | 32 | 32 |
| `VM_YIELD()` | Called after every page transferred to or from storage | — | — | `yield()` on ESP8266, nothing elsewhere |

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

If the handler returns, the failed operation does nothing; for example, `push_back` does not add the element. Element access then returns a placeholder object with meaningless contents. `VMManager::instance().last_error()` returns the last error. Two errors are not recoverable this way: after `StorageIO` the affected page's contents are lost, and after `TooManyPins` caused by deeply nested containers data may be corrupted, so stop or reset the board for those.

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

**Containers inside containers.** A container can be stored inside virtual memory, such as a `VMString` member of a vector element or a vector of vectors. Each operation keeps in RAM the page of the container itself and of the objects passed to it, and every element access marks the container's page as used, so a reference to an inner container stays valid while you keep using it:

```cpp
const VMVector<long>& row = rows[2];
for (size_t i = 0; i < row.size(); ++i) total += row[i]; // fine
```

An operation needs up to 4 resident pages at once. Deeper nesting, or an operation between two inner containers on different pages (copying or comparing `rows[1]` and `rows[3]`), can need one or two more. If there are not enough, the operation reports `VMError::TooManyPins` and, where it can, stops before changing anything; the default handler stops the program there. A handler that returns may see data corrupted in the cases the library cannot stop.

**Limits.** A `VMArray` or `VMPtr` object must fit in one page. `VMVector` elements must fit in one page each; the vector itself can span all pages.

**Long strings.** A `VMString` of up to `VMString::max_flat_size()` characters (one page minus a few bytes: 123 on AVR, 4087 on ESP32) is stored in one block, and `c_str()` returns its characters. A longer one is stored in whole pages, like a large vector, so it can be longer than the RAM: every member works the same (`s[i]`, `+=`, `insert`, `find`, `compare`, iterators, `<algorithm>`), and pages are loaded as its characters are used. Only `c_str()` cannot work, because no single piece of memory holds all the characters: on a long string (`is_flat()` false) it reports `VMError::TooLarge` and returns `""`. Read a long string with `s[i]`, a loop over its characters or `copy()`:

```cpp
VMString log;                                  // grows past one page as lines are added
log += line;
if (log.find("ERROR") != VMString::npos) { ... }
for (char c : log) Serial.write(c);            // works for any length
```

A long string goes back to one block, with `c_str()`, when it is cleared, or on `shrink_to_fit()` once it fits.

**Element types.** Pages are written to storage and read back byte by byte, and may come back to another place in RAM, so an object in virtual memory moves without its copy or move constructor being called. Store types whose bytes can be moved: numbers, structs, pointers to other memory, and MicroSwap's own containers. A type that points into itself breaks when its page moves: `std::string` of the GCC library (used on ESP32, RP2040, STM32 and Linux) keeps short strings in itself and points to them, and so do `std::function` and many classes with a small built-in buffer. On an ESP32, a `VMVector<std::string>` gave wrong strings and then crashed in `free()` once its pages had moved. Arduino `String` and `std::vector` can be moved, but their characters and elements stay in the normal heap, so they save no RAM: use `VMString` and `VMVector`.

**Several tasks (ESP32, RP2040, FreeRTOS).** The library is not thread-safe, and two things matter when more than one task uses it:

- *Use one lock for the whole library, not one per container.* All containers share the same `VMManager`: its page table, its pages in RAM and its usage counter. Two tasks working on two different vectors still change the same tables at the same time.
- *Keep the lock while you use references.* `v[i]`, `*it` and `c_str()` point into a page in RAM, and another task can swap that page out. Taking the lock only around each call is not enough: hold it from getting a reference until you are done with it.

```cpp
SemaphoreHandle_t vmLock = xSemaphoreCreateMutex(); // one for all containers

void worker(void*) {
    for (;;) {
        xSemaphoreTake(vmLock, portMAX_DELAY);
        long& x = values[0];   // valid only while the lock is held
        x += 1;
        xSemaphoreGive(vmLock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

The library does not lock by itself: a lock per call would cost every element access time without making references safe.

**Data does not survive a reset.** `begin()` starts with empty memory.

## Speed

Measured on a simulated 16 MHz ATmega (simavr, cycle-accurate) with the AVR defaults, and swap storage in RAM. This is the library's own cost; with an SD card, every page loaded or written also costs the card's transfer time, typically a few milliseconds.

| Operation | Time |
|---|---|
| Read from a plain `long[]` (for comparison) | 1.1 µs |
| `v[i]`, page already in RAM | 20 µs |
| `v[i]`, sequential over a vector 8× larger than RAM | 24 µs |
| `push_back` | 44 µs |
| `pop_back` | 9 µs |
| `VMString` `s[i]` | 16 µs |
| `VMString::push_back` | 52 µs |
| `make_vm` + `destroy` | 102 µs |
| Swapping one page in and out (without the storage time) | 175 µs |

On 32-bit boards element access is much cheaper, but the storage time dominates as soon as pages swap. So the rule is the same everywhere: work on nearby elements together, and give the library enough resident pages for the data you use at once.

## API reference

Every public member, written as class declarations. Only behavior that differs from the standard C++ library is commented. All containers allocate memory on first use, so they can be global variables created before `VMManager::begin()`.

### Errors

```cpp
enum class VMError : uint8_t {
    None,
    NotStarted,   // VMManager::begin() has not been called
    OutOfPages,   // every virtual page is in use
    TooLarge,     // the object does not fit in one page
    StorageIO,    // reading or writing the swap file failed
    OutOfRange,   // index out of range: at(), front()/back()/pop_back() on empty, ...
    NullPointer,  // a null VMPtr was dereferenced
    TooManyPins,  // too few unpinned resident pages: a VMPin over the limit, or an operation needs more pages than are free
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
    virtual bool read(uint32_t offset, uint8_t* buf, size_t len) = 0;
    virtual bool write(uint32_t offset, const uint8_t* buf, size_t len) = 0;
    virtual bool flush();   // called by VMManager::flush_all(); default does nothing

protected:
    ~VMStorage();           // not virtual, so no malloc is linked: do not delete through VMStorage*
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

// Storage on two functions of your own (and optionally a flush function).
class VMCustomStorage : public VMStorage {
public:
    typedef bool (*ReadFunction)(void* context, uint32_t offset, uint8_t* buf, size_t len);
    typedef bool (*WriteFunction)(void* context, uint32_t offset, const uint8_t* buf, size_t len);
    typedef bool (*FlushFunction)(void* context);
    VMCustomStorage(ReadFunction read, WriteFunction write, void* context = nullptr, FlushFunction flush = nullptr);
};

// MicroSwapSpiMemory.h: storage on an SPI memory chip.
struct VMSpiChip {
    constexpr VMSpiChip(uint32_t capacity, uint8_t address_bytes, uint32_t max_clock_hz, uint8_t flags = 0,
                        uint16_t max_select_us = 0, uint16_t page_bytes = 0, uint8_t read_command = 0x03,
                        uint8_t write_command = 0x02, uint8_t read_dummy_bytes = 0);
};
namespace VMSpiChips { /* LC1024, LC512, K256, K640, APS6404, MB85RS64V, FM25V10 */ }
class VMSpiMemoryStorage : public VMStorage {
public:
    VMSpiMemoryStorage(uint8_t cs_pin, const VMSpiChip& chip, uint32_t clock_hz = 0, SPIClass& spi = SPI);
    bool begin();                  // false: bad description, no chip, smaller than described, too small
    const VMSpiChip& chip() const;
    uint32_t capacity() const;
};
```

### VMManager

```cpp
class VMManager {
public:
    static VMManager& instance();

    bool begin(VMStorage& storage);   // discards all previous data: clear or destroy containers holding data first
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

### VMRingBuffer

```cpp
// The last capacity() elements pushed, oldest first; stored in a VMVector, so it can span pages.
// Takes memory as elements are pushed (it can be a global), gives it back on clear().
template<class T>
class VMRingBuffer {
public:
    // value_type, reference, const_reference, size_type, difference_type,
    // iterator, const_iterator, reverse_iterator, const_reverse_iterator (random-access)

    explicit VMRingBuffer(size_type capacity);

    reference       operator[](size_type i);       // 0 is the oldest
    const_reference operator[](size_type i) const;
    reference       at(size_type i);               // out of range -> VMError::OutOfRange
    const_reference at(size_type i) const;
    reference       front();                       // oldest
    reference       back();                        // newest
    iterator        begin(); iterator end();       // oldest to newest; also const, c- and r- versions

    bool      empty() const;
    bool      full() const;
    size_type size() const;
    size_type capacity() const;

    void push_back(const T& value);                // when full, overwrites the oldest
    void push_back(T&& value);
    template<class... Args> void emplace_back(Args&&... args);
    bool try_push_back(const T& value);            // false (and nothing changed) when full
    void pop_front();                              // removes the oldest, resets its slot to T(); empty -> OutOfRange
    void clear();                                  // removes everything and frees the memory
    void swap(VMRingBuffer& other);
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
// Up to max_flat_size() characters in one heap block (with c_str()); longer in whole pages.
// An empty string uses no memory.
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
    const char*     c_str() const;             // pointer into a resident page, see "Rules to follow"; only while is_flat()

    // Capacity
    bool      empty() const;
    size_type size() const;
    size_type length() const;
    size_type capacity() const;
    static constexpr size_type max_size();     // all virtual pages (VM_PAGE_COUNT * VM_PAGE_SIZE)
    bool is_flat() const;                      // not in std::string: one block, so c_str() works
    static constexpr size_type max_flat_size(); // not in std::string: longest string in one block (123 on AVR)
    void reserve(size_type new_cap);           // beyond max_size() -> VMError::TooLarge
    void shrink_to_fit();                      // frees unused pages; a long string that fits goes back to one block
    void resize(size_type n, char ch = '\0');
    void clear();

    // Modifiers (growing beyond max_size() -> VMError::TooLarge, string unchanged; running out of pages -> OutOfPages)
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

## Tested on hardware

Besides the host tests and the CI builds, the examples were run on real boards:

| Board | Swap storage | Result |
|---|---|---|
| ESP32 (ESP32-D0WDQ6, Arduino core 2.x) | SD card, HW-125 module on VSPI | Every example correct with the ESP32 defaults and with 128-byte pages, 4 of them in RAM (constant swapping) |
| ESP32 | LittleFS on the internal flash | The same |

`VMSpiMemoryStorage` is tested against simulated chips of each family (commands, addresses, chip select, write enable, PSRAM pages and chip-select time), not yet on real ones.

No resets or watchdog timeouts. Not run on a board yet: AVR (Uno, Mega), ESP8266.

## Running tests

The library can be tested on a desktop machine without any board:

```sh
make -C tests
```

The tests are built in four configurations, with sanitizers: the default one; two AVR-like ones (C++11, no STL, 128-byte pages, 4 resident pages), where pages are swapped constantly; and one where pins leave a single page free. GitHub Actions runs them and compiles the examples for every board listed above.

## Upgrading from earlier versions

- Include `<MicroSwap.h>` instead of `containers.h`.
- `VMManager::begin(fs, path)` became `begin(VMStorage&)`: open the file yourself and wrap it in `VMFileStorage<File>`.
- Errors no longer throw exceptions; see [Errors](#errors).
- `VMPtr` no longer supports pointer arithmetic, and a default-constructed `VMPtr` is null instead of allocating on first use.
- `VM_PAGE_COUNT` now counts virtual pages; only `VM_RESIDENT_PAGES` of them are in RAM.

## License

Public domain ([The Unlicense](LICENSE)).
