# Changelog

## 0.2.0 (unreleased)

First release as an Arduino / PlatformIO library. The core was rewritten, so code written for the earlier `containers.h` needs small changes (see "Upgrading" in the README).

### Added
- Runs on any Arduino core with a C++11 compiler: AVR, ESP32, ESP8266, STM32, SAMD, Renesas, RP2040. It uses no exceptions, no RTTI, no `malloc`, and no STL; the STL is used when available, so iterators work with `<algorithm>`. All of its RAM is static.
- Real swapping: a fixed pool of `VM_RESIDENT_PAGES` pages in RAM, with least-recently-used eviction, over `VM_PAGE_COUNT` virtual pages.
- `VMStorage` interface for swap storage, and `VMFileStorage<File>` for any Arduino `File` (SD, SdFat, SDFS, ...).
- `VMPin<T>` and `vm_pin()` to keep a page in RAM while a reference to it is held.
- Error handler (`set_error_handler`, `last_error`, `vm_error_name`) instead of exceptions.
- Statistics: `free_pages()`, `swap_in_count()`, `swap_out_count()`.
- Per-platform defaults for `VM_PAGE_SIZE`, `VM_RESIDENT_PAGES`, `VM_PAGE_COUNT`, `VM_HEAP_ALIGN`. `VM_RESIDENT_PAGES` must be at least 4.
- `library.properties` and `library.json`; examples for every container, pinning, error handling, and custom storage.
- `VM_YIELD()` hook, called after every page transfer: on ESP8266 it calls `yield()`, so long operations do not trip the watchdog.
- Host tests (four configurations, sanitizers) and CI builds for 15 boards with Arduino CLI and 5 with PlatformIO.
- Run on a real ESP32 with an SD card and with LittleFS (README, "Tested on hardware").
- README: which element types can live in virtual memory (not `std::string` of the GCC library, or anything else that points into itself), and notes on SD modules.
- The examples try `SD.begin()` up to three times on ESP32: the first one after a reset can fail.
- `VMSpiMemoryStorage` (`MicroSwapSpiMemory.h`): swap storage on SPI memory chips, described by a `VMSpiChip` (size, address length, clock, write enable, mode register, reset, chip-select time, page size, commands). `VMSpiChips` has 23LC1024, 23LC512, 23K256, 23K640 SRAM, APS6404 PSRAM and MB85RS64V, FM25V10 FRAM; other chips are described the same way. `begin()` checks that the chip answers and holds what was described. SpiMemory example. Tested against simulated chips; not yet on real ones.
- `VMMap<Key, T>`: a map sorted by key, like `std::map`: `find`, `contains`, `count`, `lower_bound`, `upper_bound`, `at`, `operator[]`, `insert`, `emplace`, `insert_or_assign`, `erase`, iteration in key order. Entries are kept sorted in a `VMVector`, so lookups are a binary search and keys inserted in ascending order are appended; with `VMString` keys, lookups by C string allocate nothing. `VMPair` stands in for `std::pair`. Map example.
- `VMRingBuffer<T>`: a fixed-capacity ring buffer in virtual memory. `push_back` overwrites the oldest element when full (the last N readings), `try_push_back` refuses instead (a queue); `pop_front`, index 0 is the oldest, iterators work with `<algorithm>`. It takes memory as it fills and gives it back on `clear()`. RingBuffer example.
- `VMCustomStorage`: swap storage on two functions of your own, without writing a class; the CustomStorage example uses it.
- `VMString` of any length up to `max_size()` (all virtual pages): a string longer than `max_flat_size()` is stored in whole pages and loaded as its characters are used, so it can be longer than the RAM. Every member works on it except `c_str()`, which needs one block (`is_flat()`); on a long string it reports `TooLarge` and returns `""`. `shrink_to_fit()` brings a long string that fits back to one block. About 1.7 KB more flash on an Uno, no more RAM.

### Changed
- Include `<MicroSwap.h>` instead of `containers.h`.
- `VMManager::begin(fs, path)` became `begin(VMStorage&)`.
- A default-constructed `VMPtr` is null; it no longer allocates on first use. Pointer arithmetic was removed.
- `VMArray` allocates on first access, so it can be a global variable.
- An empty `VMString` uses no memory.
- Pages are written back only if they changed; pages read through non-const references are compared with their stored copy first.
- Storage is flushed only by `flush_all()`.
- `VMVector` no longer allocates a page table with `malloc`: its pages are linked through the page table that `VMManager` already has, and the last page looked up is remembered, so sequential access stays fast. Without SD, a sketch using `VMVector` is about 700 bytes smaller on AVR. `VMError::OutOfMemory` was removed.
- `VMStorage` has a protected, non-virtual destructor, so that no `operator delete` (and with it `malloc`) is linked.
- On AVR the least-recently-used counter is 16 bits (`VM_TICK_BITS`), renumbered before it overflows: about 260 bytes less flash and 10 bytes less RAM on an Uno.
- The `VMManager` is a constant-initialized static object instead of a function-local static, so `instance()` has no "already constructed?" check and is inlined: about 270–440 bytes less flash and 8 bytes less RAM on an Uno. It is valid before any code runs, so global objects may use it from their constructors.

### Fixed
- `reserve()` before `push_back()` shifted elements by one page.
- `pop_back()` at a page boundary destroyed the wrong element.
- `swap()` did not exchange all of the vector's state.
- Running out of pages wrote through a null pointer.
- Leaks of non-trivial elements when a vector grew.
- Heap fragmentation: free blocks are merged and empty heap pages are released.
- Copies of `VMArray` shared storage and freed it twice.
- References became invalid after the LRU counter overflowed.
- Writes made after `flush_all()` through earlier references could be lost.
- Changing a container stored inside virtual memory (`people[i].name.append(...)`, `rows[i].reserve(n)`) could crash or corrupt data when the operation loaded other pages. Each operation now keeps the pages of the container and of its arguments in RAM.
- `v.assign(4, 1)` chose the iterator-range overload and did not compile.
- `v.push_back(v[0])` (or `emplace_back`, `insert`) on a vector that had to grow copied the element after it had been moved.
- `find_first_of` and the related searches matched a `'\0'` inside the string against any character set.
- Using an inner container through a reference (`const auto& row = rows[i]; ... row[j]`) could read its fields after its page had been evicted.
- Copying one inner container to another (`rows[3] = rows[1]`) could fill the copy with zeros when too few pages were free, without reporting an error. Copies now reserve space first, and every step that needs two pages at once checks that they are available.
