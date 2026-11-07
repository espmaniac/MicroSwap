# MicroSwap

**More memory than your board has.** MicroSwap gives Arduino boards vectors, strings, maps and ring buffers that are much bigger than their RAM. It keeps a few pages of data in RAM and the rest on an SD card, in an SPI memory chip or in flash, and moves pages back and forth by itself as you use the data.

```cpp
VMVector<long> readings;                 // 4 KB of data on an Uno, which has 2 KB of RAM
for (long i = 0; i < 1000; ++i) readings.push_back(i * i);
Serial.println(readings[999]);           // 998001 — the page is loaded back by itself
```

MicroSwap itself takes about 0.7 KB of RAM on an Uno; raise `VM_PAGE_COUNT` for more virtual memory (see [Configuration](#configuration)).

- Works like the standard containers: `push_back`, `insert`, `find`, `for (auto x : v)`, `std::sort`.
- Any Arduino board with a C++11 compiler: AVR (Uno, Mega), ESP32, ESP8266, RP2040, STM32, SAMD, Teensy and more.
- No `malloc`, no exceptions: all of its RAM is reserved at compile time, so the IDE shows it.
- Costs nothing until you use it: including the header adds no byte of flash or RAM.

## Quick start

1. Install the library (see [Installation](#installation)) and wire an SD card module to the SPI pins.
2. Open the swap file, start MicroSwap, use the containers:

```cpp
#include <SPI.h>
#include <SD.h>
#include <MicroSwap.h>

File swapFile;
VMFileStorage<File> swapStorage(swapFile);   // MicroSwap's pages go to this file

void setup() {
    Serial.begin(115200);
    SD.begin(SS);                            // your card's chip select pin

    SD.remove("/swap.bin");                  // start with an empty swap file
    swapFile = SD.open("/swap.bin", O_READ | O_WRITE | O_CREAT);   // ESP32: SD.open("/swap.bin", "w+")
    VMManager::instance().begin(swapStorage);

    VMVector<long> v;
    for (long i = 0; i < 1000; ++i) v.push_back(i * i);
    Serial.println(v[999]);                  // 998001
}

void loop() {}
```

The examples use a ready `SwapFile.h` (in each example's folder) that opens the file the right way on every board: copy it into your sketch and call `beginSwapOrHalt()`.

## Which container do I need?

| You want | Use | Like |
|---|---|---|
| A list that grows: readings, records, a buffer | `VMVector<T>` | `std::vector` |
| Text, of any length: messages, logs, JSON | `VMString` | `std::string` |
| The last N values (newest replaces oldest), or a queue | `VMRingBuffer<T>` | a circular buffer |
| Look up by key: settings, names, counts | `VMMap<Key, T>` | `std::map` |
| A fixed number of elements, e.g. a global table | `VMArray<T, N>` | `std::array` |
| One big object (a struct, a buffer) | `VMPtr<T>` with `make_vm<T>(...)` | a pointer (copies share the object) |

Mostly you can take code written for the standard containers and change the type: `std::vector<long>` becomes `VMVector<long>`. Read [the three rules](#the-three-rules) first.

## Where do the pages go?

MicroSwap needs one storage for its pages. Pick one:

| Storage | Good | Watch out |
|---|---|---|
| **SD card**, a file (`VMFileStorage<File>`) | gigabytes; any SD library (SD, SdFat, SDFS) | the SD library takes flash and RAM; slower |
| **SPI memory chip**: SRAM, PSRAM, FRAM (`VMSpiMemoryStorage`) | fast, does not wear out, small code (no file system) | one chip, 8 KB to 8 MB |
| **Internal flash**, LittleFS / SPIFFS (`VMFileStorage<File>`) | nothing to wire | wears out under constant rewrites: for tests, or with many pages in RAM |
| **Anything else** (`VMCustomStorage`, two functions) | your own memory or protocol | you write the read and write |

On an Uno the SPI memory chip is the lightest choice: the SpiMemory example takes 9.2 KB of flash and 1 KB of RAM, the Basic example with an SD card 20 KB and 1.8 KB.

### An SD card or a flash file

The file must be opened for reading **and** writing, and stay open:

| Library | Open with |
|---|---|
| ESP32 `SD`, `LittleFS` | `SD.open(path, "w+")` |
| ESP8266 / RP2040 `SDFS` | `SDFS.open(path, "w+")` |
| Arduino `SD` (AVR, SAMD, STM32, ...) | `SD.open(path, O_READ \| O_WRITE \| O_CREAT)` |
| SdFat | `sd.open(path, O_RDWR \| O_CREAT \| O_TRUNC)` |

Do not use `FILE_WRITE` with the Arduino `SD` library: it appends every write to the end of the file.

Tips for SD card modules:
- On an ESP32 the first `SD.begin()` after a reset can fail: the reset does not cut the card's power, and the card may still be busy. Call it again; the examples try three times.
- Many modules (HW-125 and alike) have their own 3.3 V regulator and need **5 V** on VCC.
- Many do not release MISO when the card is not selected: give such a module an SPI bus of its own.
- With long wires, keep the SPI clock at 10 MHz or less.

### An SPI memory chip

Include `MicroSwapSpiMemory.h` and pick your chip:

```cpp
#include <MicroSwapSpiMemory.h>

VMSpiMemoryStorage memory(10, VMSpiChips::LC1024);   // chip select pin, chip

void setup() {
    if (!memory.begin()) {
        // no chip answers, it is smaller than described, or the virtual memory does not fit
    }
    VMManager::instance().begin(memory);
}
```

| `VMSpiChips::` | Chip | Size | Supply |
|---|---|---|---|
| `LC1024` | Microchip 23LC1024 / 23A1024 SRAM | 128 KB | 2.5–5.5 V |
| `LC512` | Microchip 23LC512 / 23A512 SRAM | 64 KB | 2.5–5.5 V |
| `K256`, `K640` | Microchip 23K256, 23K640 SRAM | 32 KB, 8 KB | 2.7–3.6 V |
| `APS6404` | APS6404L / ESP-PSRAM64H / LY68L6400 PSRAM | 8 MB | 2.7–3.6 V |
| `APS6404X` | the same, extended temperature grade (to 105 °C) | 8 MB | 2.7–3.6 V |
| `MB85RS64V` | Fujitsu MB85RS64V FRAM | 8 KB | 3.0–5.5 V |
| `FM25V10` | Infineon FM25V10 FRAM | 128 KB | 2.0–3.6 V |

The descriptions follow the datasheets; on real chips they are not tested yet (see [Tested on hardware](#tested-on-hardware)). The virtual memory (`VM_PAGE_SIZE * VM_PAGE_COUNT`) must fit in the chip. `begin()` checks the chip; the SPI clock defaults to 8 MHz, never above the chip's (pass another as the third argument), and other devices can share the SPI bus.

**A chip that is not in the list.** Chips differ only in a few numbers, so describe yours — no code needed:

```cpp
// capacity, address bytes (2, 3 or 4), max clock, flags,
// then optionally: max chip-select time (µs), page bytes, read command, write command, dummy bytes after a read's address
const VMSpiChip myChip(65536UL, 2, 20000000UL, kVMSpiWriteEnable);
VMSpiMemoryStorage memory(10, myChip);
```

Flags: `kVMSpiModeRegister` sets sequential mode at start (Microchip SRAM); `kVMSpiWriteEnable` sends write enable before every write (FRAM, MRAM); `kVMSpiReset` resets the chip at start (PSRAM). PSRAM also needs its chip-select time and its page size: transfers are split to respect both, so a faster SPI clock makes it faster. Flash and EEPROM chips are not for swapping: they need erasing or slow page writes, and they wear out.

### Storage of your own

For any other memory, give MicroSwap two functions:

```cpp
bool myRead(void* context, uint32_t offset, uint8_t* buf, size_t len)        { /* ... */ return true; }
bool myWrite(void* context, uint32_t offset, const uint8_t* buf, size_t len) { /* ... */ return true; }

VMCustomStorage storage(myRead, myWrite);   // optional: a context passed to both, a flush function
VMManager::instance().begin(storage);
```

Each returns `false` if the transfer failed. Or implement the `VMStorage` interface (see [Swap storage](#swap-storage)).

## The three rules

**1. Use a reference right away.** `v[i]`, `*it`, `*ptr` and `s.c_str()` point into a page in RAM. When other pages are loaded later, that page may leave RAM and the reference then points to other data:

```cpp
long& first = v[0];
for (size_t i = 0; i < v.size(); ++i) total += v[i];   // may move v[0]'s page out
first = 5;                                             // wrong: `first` may be stale

v[0] = 5;                                              // right
```

To keep a reference longer, pin its page with `VMPin`:

```cpp
VMPin<long> first(v[0]);                               // or: auto first = vm_pin(v[0]);
for (size_t i = 0; i < v.size(); ++i) total += v[i];
*first = 5;                                            // right: the page stayed in RAM
```

At most `VM_RESIDENT_PAGES - 4` pages can be pinned at once (the containers need 4 for their own work): one by default, four on ESP32, none on AVR unless you raise `VM_RESIDENT_PAGES` to 5. A pin that does not fit reports `TooManyPins` and `valid()` is false. Do not `push_back`, `insert`, `erase` or destroy a container while a pin into it lives.

**2. Store only types whose bytes can move.** Pages go to storage and come back byte by byte, often to another place in RAM, so a stored object moves without its copy constructor. Numbers, structs, pointers to other memory and MicroSwap's own containers are fine. A type that points into itself breaks: `std::string` of the GCC library (ESP32, RP2040, STM32, Linux) does, and so do `std::function` and many classes with a small built-in buffer — on an ESP32 a `VMVector<std::string>` gave wrong strings and crashed. Arduino `String` and `std::vector` do move fine, but keep their contents in the normal heap, so they save no RAM. Use `VMString` and `VMVector`.

**3. With several tasks, use one lock for the whole library.** All containers share the same pages and tables, so two tasks working on two different vectors still touch the same data. Hold the lock from getting a reference until you are done with it:

```cpp
SemaphoreHandle_t vmLock = xSemaphoreCreateMutex();   // one for all containers

void worker(void*) {
    for (;;) {
        xSemaphoreTake(vmLock, portMAX_DELAY);
        long& x = values[0];                           // valid only while the lock is held
        x += 1;
        xSemaphoreGive(vmLock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

The library does not lock by itself: a lock per call would slow every access without making references safe.

Also good to know: **data does not survive a reset** — `begin()` starts with empty memory.

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

**Arduino CLI.** `arduino-cli lib install --git-url https://github.com/espmaniac/MicroSwap.git` (needs `library.enable_unsafe_install` set to `true`).

## Examples

| Example | Shows |
|---|---|
| [Basic](examples/Basic/Basic.ino) | A vector bigger than RAM, a string, swap statistics |
| [Vector](examples/Vector/Vector.ino) | Every `VMVector` operation: insert, erase, iterators, flat and paged storage, resize, copy, swap, compare |
| [String](examples/String/String.ino) | Building, searching, extracting, editing and comparing `VMString`, and a long string |
| [RingBuffer](examples/RingBuffer/RingBuffer.ino) | The last 2000 sensor readings in a `VMRingBuffer`: average, minimum, maximum, and use as a queue |
| [Map](examples/Map/Map.ino) | Counting words with a `VMMap<VMString, int>`, and a table by number: insert, update, erase, lookups by C string |
| [Array](examples/Array/Array.ino) | `VMArray` as a global, arrays of structs, fill, copy, iterators |
| [Pointer](examples/Pointer/Pointer.ino) | `make_vm`, `VMPtr` as a long-lived object used from `loop()`, `destroy()` |
| [Pin](examples/Pin/Pin.ino) | Keeping references and C strings valid with `VMPin`, and the pin limit |
| [ErrorHandling](examples/ErrorHandling/ErrorHandling.ino) | A custom error handler and every kind of error |
| [Algorithms](examples/Algorithms/Algorithms.ino) | `std::sort`, `std::accumulate`, `std::transform` on containers (not on AVR) |
| [SpiMemory](examples/SpiMemory/SpiMemory.ino) | Swapping to an SPI memory chip (23LC1024 SRAM, PSRAM, FRAM) |
| [CustomStorage](examples/CustomStorage/CustomStorage.ino) | A storage of your own on two functions, with `VMCustomStorage` |

All examples except SpiMemory and CustomStorage put the swap file on an SD card, through `SwapFile.h` in the example's folder. Set `SD_CS_PIN` there to your card's chip select pin.

## Configuration

Set these as build flags (`build_flags = -DVM_RESIDENT_PAGES=6` in PlatformIO), or with `#define` before `#include <MicroSwap.h>` in every file that includes it:

| Macro | Meaning | AVR | ESP32 | Others |
|---|---|---|---|---|
| `VM_PAGE_SIZE` | Bytes per page; also the largest array or object | 128 | 4096 | 1024 |
| `VM_RESIDENT_PAGES` | Pages kept in RAM (4 to 254) | 4 | 8 | 5 |
| `VM_PAGE_COUNT` | Total virtual pages | 48 | 256 | 128 |
| `VM_HEAP_ALIGN` | Alignment of stored objects | 2 | 8 | 8 |
| `VM_TICK_BITS` | Width of the least-recently-used counter (16 or 32) | 16 | 32 | 32 |
| `VM_YIELD()` | Called after every page transferred to or from storage | — | — | `yield()` on ESP8266, nothing elsewhere |

How much memory that is:
- **RAM:** about `VM_PAGE_SIZE * VM_RESIDENT_PAGES + 4 * VM_PAGE_COUNT` bytes — 0.7 KB on AVR, 33 KB on ESP32 — reserved as soon as you use MicroSwap, however many containers you have.
- **Virtual memory:** `VM_PAGE_SIZE * VM_PAGE_COUNT` — 6 KB on AVR, 1 MB on ESP32; the swap file grows up to that.
- **More resident pages** mean fewer swaps; **smaller pages** mean less data moved per swap.

A page is written back only if it changed, so reading does not wear the storage.

## Errors

Errors — out of pages, a storage failure, an index out of range — go to an error handler. The default one calls `abort()`. Set your own to log and reset, or to carry on:

```cpp
void onVMError(VMError error) {
    Serial.print(F("MicroSwap error: "));
    Serial.println(vm_error_name(error));
}

VMManager::instance().set_error_handler(onVMError);
```

If the handler returns, the failed operation does nothing (`push_back` does not add the element), element access returns a placeholder with meaningless contents, and `VMManager::instance().last_error()` tells what happened. Two errors cannot be recovered this way: after `StorageIO` the affected page is lost, and after `TooManyPins` from deeply nested containers data may be corrupted, so stop or reset the board for those.

## More about the containers

**Long strings.** A `VMString` of up to `VMString::max_flat_size()` characters (123 on AVR, 4087 on ESP32) is one block, and `c_str()` gives its characters. A longer one is stored in pages like a big vector, so it can be longer than the RAM; everything works the same (`s[i]`, `+=`, `insert`, `find`, `compare`, iterators) except `c_str()`, because no single piece of memory holds all the characters. On a long string (`is_flat()` false) `c_str()` reports `TooLarge` and returns `""`; read it character by character or with `copy()`:

```cpp
VMString log;                                  // grows past one page as lines are added
log += line;
if (log.find("ERROR") != VMString::npos) { ... }
for (char c : log) Serial.write(c);            // works for any length
```

A long string becomes one block again when cleared, or on `shrink_to_fit()` once it fits.

**Maps.** `VMMap` keeps its entries sorted in a vector: a lookup is a binary search, and keys inserted in ascending order are simply appended, but inserting in the middle moves the entries after it. It suits tables read more than changed. With `VMString` keys, `find("name")` compares with the C string directly and allocates nothing.

**Containers inside containers.** A `VMString` member of a vector element, a vector of vectors, maps in a vector: all work. Each operation keeps in RAM the container and the objects passed to it, so a reference to an inner container stays valid while you use it:

```cpp
const VMVector<long>& row = rows[2];
for (size_t i = 0; i < row.size(); ++i) total += row[i];   // fine
```

An operation needs up to 4 resident pages; deeper nesting, or an operation between two inner containers on different pages (copying `rows[1]` to `rows[3]`), can need one or two more. Without them it reports `TooManyPins` and, where it can, stops before changing anything.

**Limits.** A `VMArray` or `VMPtr` object must fit in one page. Each `VMVector` element must fit in one page; the vector itself can span all pages.

**Algorithms.** The iterators are random-access, so `<algorithm>` works where the board has the STL (every board in [Supported boards](#supported-boards) except AVR, megaAVR and SAM). `std::sort` over numbers and over objects with `VMString` members is covered by the tests; algorithms or comparators that hold many objects at once may need a larger `VM_RESIDENT_PAGES`.

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
namespace VMSpiChips { /* LC1024, LC512, K256, K640, APS6404, APS6404X, MB85RS64V, FM25V10 */ }
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

### VMMap

```cpp
// Entries VMPair<Key, T> kept sorted by key in a VMVector: lookups are a binary search,
// inserting or erasing in the middle moves the entries after it (keys in ascending order are appended).
// Suits tables read more than changed. Do not change an entry's first through an iterator.
template<class Key, class T, class Compare = VMLess>   // VMLess: operator<, and VMString against C strings
class VMMap {
public:
    // key_type, mapped_type, value_type (VMPair<Key, T>), size_type, difference_type,
    // iterator, const_iterator, reverse_iterator, const_reverse_iterator (random-access, in key order)

    // Lookups take any key comparable with Key: with VMString keys, find("name") allocates nothing.
    template<class K> iterator  find(const K& key);            // end() if missing; also const
    template<class K> bool      contains(const K& key) const;
    template<class K> size_type count(const K& key) const;     // 0 or 1
    template<class K> iterator  lower_bound(const K& key);     // first key not less than key; also const
    template<class K> iterator  upper_bound(const K& key);     // first key greater than key; also const
    template<class K> T&        at(const K& key);              // missing -> VMError::OutOfRange; also const
    template<class K> T&        operator[](const K& key);      // inserts T() if missing

    template<class K, class V> VMPair<iterator, bool> emplace(K&& key, V&& value);          // .second: inserted
    template<class K, class V> VMPair<iterator, bool> insert(K&& key, V&& value);           // an existing key stays
    VMPair<iterator, bool> insert(const value_type& entry);
    template<class K, class V> VMPair<iterator, bool> insert_or_assign(K&& key, V&& value); // replaces an existing value
    template<class K> size_type erase(const K& key);           // 0 or 1
    iterator erase(iterator pos);
    iterator erase(const_iterator pos);

    bool      empty() const;
    size_type size() const;
    void      reserve(size_type n);
    void      clear();                                         // frees the memory
    void      swap(VMMap& other);
    // begin(), end(), cbegin(), cend(), rbegin(), rend()
};

template<class A, class B> struct VMPair { A first; B second; };   // like std::pair
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

## Supported boards

Any Arduino core with a C++11 compiler. The library uses no exceptions, no RTTI and no `malloc` (all of its RAM is static, so the IDE reports it at compile time), and needs no STL or `FS.h`. Every change is compiled in CI for:

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

## Tested on hardware

Besides the host tests and the CI builds, the examples were run on real boards:

| Board | Swap storage | Result |
|---|---|---|
| ESP32 (ESP32-D0WDQ6, Arduino core 2.x) | SD card, HW-125 module on VSPI | Every example correct with the ESP32 defaults and with 128-byte pages, 4 of them in RAM (constant swapping), including RingBuffer, Map and a long string; an 8000-character string edited 60 times in the middle with 512 bytes of pages in RAM |
| ESP32 | LittleFS on the internal flash | Every example correct |

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
