# Ideas

Things that could be added later. None of them is planned or promised; each
says why it might be worth it and what to watch out for.

## Storage

- **`FILE*` storage (C stdio).** A ready `VMStorage` over `fopen` / `fread` /
  `fwrite` / `fseek`. Useful outside Arduino: ESP-IDF without Arduino (files on
  SD and flash go through its VFS with plain `fopen`), embedded Linux, and
  testing a sketch's logic on a computer before flashing it. Today the same can
  be done with `VMCustomStorage` and two small functions.
- **Serial storage.** Pages kept in a computer's RAM over USB serial, with a
  small script on the computer answering read and write requests. Only a
  debugging tool: the board stops working when the computer or the script
  goes away, and the port cannot be used for `Serial.print` at the same time.
  Not planned as part of the library; anyone who wants it can build it on
  `VMCustomStorage`.
- **More SPI chips in `VMSpiChips`.** MRAM (MR25H and alike), more FRAM sizes.
  Each description should be checked against its datasheet, and against a real
  chip before it is called tested.

## Platforms

- **Outside Arduino.** `MicroSwap.h` needs only a C++11 compiler and the C
  library, so it builds in ESP-IDF, Zephyr, STM32 HAL, mbed or RP2040 SDK
  projects; storage comes from `VMCustomStorage` or a `VMStorage` of your own.
  Not tested there yet: an ESP-IDF component example and a note in the README
  would make it official. On a computer the library is a development tool (run
  and debug a sketch's logic with sanitizers), not a reason to use it: the
  operating system already swaps.
- **Hardware not run yet.** Arduino Uno and Mega, ESP8266 (where `VM_YIELD()`
  keeps the watchdog quiet), and real SPI SRAM, PSRAM and FRAM chips.

## Containers

- **B-tree map.** `VMMap` is a sorted vector: fast lookups, but inserting in
  the middle moves the entries after it. A B-tree with one node per page would
  make inserts and erases cheap in big tables; worth it only if such tables
  appear.
- **`VMString` from and to Arduino `String`.**
- **A switch to leave long-string code out** (`VM_LONG_STRINGS=0`), for sketches
  that are short of flash on an Uno: about 1.7 KB.

## Safety and tuning

- **A debug mode that catches stale references** (`VM_DEBUG`): a generation
  counter per page, so using `v[i]` after its page left RAM is reported instead
  of reading other data.
- **Statistics for tuning**: how many accesses found their page in RAM, how many
  pages were written back. Helps pick `VM_PAGE_SIZE` and `VM_RESIDENT_PAGES`.
- **Data that survives a reset.** On FRAM or SD the pages are still there after
  a reset, but the page table lives in RAM. Storing it too would make
  containers that survive power loss (data loggers); it needs care for power
  lost in the middle of a write.
- **Faster element access**: remember the last page's slot in each container.
  Only if speed becomes a problem.
