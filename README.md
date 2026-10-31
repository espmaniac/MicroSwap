# MicroSwap

Virtual memory with a swap file, and STL-like containers on top of it, for Arduino boards with little RAM.

MicroSwap keeps a few pages of data in RAM and stores the rest in a file on an SD card or flash. Pages are loaded back automatically when you access them, and the least recently used page is written out to make room. An Arduino Uno can hold a `VMVector<long>` of thousands of elements while using about 0.5 KB of RAM for data.

- `VMVector<T>`: vector, works with `std::sort` and other algorithms where the STL exists
- `VMArray<T, N>`: fixed-size array
- `VMString`: mutable string
- `VMPtr<T>` + `make_vm<T>(...)`: one object in virtual memory

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

Download the repository as a ZIP and use *Sketch → Include Library → Add .ZIP Library* in the Arduino IDE, or clone it into your `libraries/` folder. With PlatformIO, add the repository URL to `lib_deps`.

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

See [`examples/Basic`](examples/Basic/Basic.ino) for a sketch that picks the right file system on each board, and [`examples/Algorithms`](examples/Algorithms/Algorithms.ino) for `<algorithm>` usage.

### Opening the swap file

The file must be opened for both reading and writing and must stay open:

| Library | Open with |
|---|---|
| ESP32 `SD`, `LittleFS`, `SPIFFS` | `SD.open(path, "w+")` |
| ESP8266 / RP2040 `LittleFS` | `LittleFS.open(path, "w+")` |
| Arduino `SD` (AVR, SAMD, STM32, ...) | `SD.open(path, O_READ \| O_WRITE \| O_CREAT)` |
| SdFat | `sd.open(path, O_RDWR \| O_CREAT \| O_TRUNC)` |

Do not use `FILE_WRITE` with the Arduino `SD` library: it appends every write to the end of the file.

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
| `VM_RESIDENT_PAGES` | Pages kept in RAM (3 to 254) | 3 | 8 | 4 |
| `VM_PAGE_COUNT` | Total virtual pages | 64 | 256 | 128 |
| `VM_HEAP_ALIGN` | Alignment of stored objects | 2 | 8 | 8 |

RAM used is about `VM_PAGE_SIZE * VM_RESIDENT_PAGES + 4 * VM_PAGE_COUNT` bytes. The swap file grows up to `VM_PAGE_SIZE * VM_PAGE_COUNT` bytes. More resident pages mean fewer swaps; smaller pages mean less data written per swap.

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

**Use references right away.** `v[i]`, `*ptr`, `s.c_str()` and iterators' `*it` return references into a page in RAM. If other pages are loaded afterwards, that page may be swapped out and the reference then points to other data. At least `VM_RESIDENT_PAGES - 1` other pages can be touched safely, which is enough for the containers' own operations and for `std::sort` and similar algorithms. Do not keep a reference while doing a lot of other work:

```cpp
long& first = v[0];
for (size_t i = 0; i < v.size(); ++i) total += v[i]; // may evict v[0]'s page
first = 5;                                         // wrong: `first` may be stale

v[0] = 5;                                          // right
```

**Limits.** A `VMString`, `VMArray` or `VMPtr` object must fit in one page (`VMString::max_size()` is `VM_PAGE_SIZE - 5` on AVR). `VMVector` elements must fit in one page each; the vector itself can span all pages.

**Not thread-safe.** Use it from one task only, or protect it with a mutex.

**Data does not survive a reset.** `begin()` starts with empty memory.

## API overview

```cpp
// Manager
VMManager& vm = VMManager::instance();
bool     begin(VMStorage& storage);
void     end();
bool     flush_all();                  // write modified pages to storage
VMError  last_error() const;
void     clear_error();
void     set_error_handler(VMErrorHandler handler);
size_t   get_page_size() const;
size_t   get_page_count() const;
size_t   get_resident_pages() const;
size_t   free_pages() const;
uint32_t swap_in_count() const;
uint32_t swap_out_count() const;

// VMVector<T>: std::vector-like
push_back, emplace_back, pop_back, insert, emplace, erase, clear, resize,
reserve, shrink_to_fit, assign, swap, operator[], at, front, back, size,
capacity, empty, begin/end, rbegin/rend, comparison operators
bool is_flat() const;   // true while all elements are in one block
T*   data();            // contiguous storage while is_flat(), otherwise nullptr

// VMArray<T, N>: std::array-like
operator[], at, front, back, size, empty, fill, clear, begin/end, rbegin/rend

// VMString: std::string-like
c_str, size, length, capacity, max_size, reserve, resize, clear, assign,
append, operator+=, push_back, pop_back, insert, erase, replace, substr, copy,
swap, find, rfind, find_first_of, find_last_of, find_first_not_of,
find_last_not_of, compare, comparison operators, operator+

// VMPtr<T>
VMPtr<Person> p = make_vm<Person>(28, "oscar");  // null VMPtr on failure
p->age++;
p.destroy();                                      // objects live until destroy()
```

Containers allocate memory on first use, so they can be global variables created before `begin()`.

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
