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
- Host tests (two configurations, sanitizers) and CI builds for 9 boards.

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
- Using an inner container through a reference (`const auto& row = rows[i]; ... row[j]`) could read its fields after its page had been evicted.
- Copying one inner container to another (`rows[3] = rows[1]`) could fill the copy with zeros when too few pages were free, without reporting an error. Copies now reserve space first, and every step that needs two pages at once checks that they are available.
