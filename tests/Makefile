CXX      ?= c++
WARNINGS  = -Wall -Wextra -Wpedantic
SANITIZE  = -fsanitize=address,undefined -fno-omit-frame-pointer

# Default configuration, C++17 with the STL.
DEFAULT_FLAGS = -std=c++17 -g $(WARNINGS) $(SANITIZE)

# AVR-like configuration: C++11 (as used by the AVR core), no STL, tiny
# pages and only four of them in RAM (the minimum), so pages are swapped constantly.
# (Real AVR uses VM_HEAP_ALIGN=2; desktop types need 8.) The LRU counter
# is renumbered every 1000 accesses instead of every 2^32.
SMALL_FLAGS = -std=c++11 -g $(WARNINGS) $(SANITIZE) \
              -DVM_USE_STL=0 -DVM_PAGE_SIZE=128 -DVM_RESIDENT_PAGES=4 \
              -DVM_PAGE_COUNT=64 -DVM_HEAP_ALIGN=8 -DVM_TICK_LIMIT=1000

test: test_default test_small
	./test_default
	./test_small

test_default: test_containers.cpp ../src/MicroSwap.h
	$(CXX) $(DEFAULT_FLAGS) -o $@ test_containers.cpp

test_small: test_containers.cpp ../src/MicroSwap.h
	$(CXX) $(SMALL_FLAGS) -o $@ test_containers.cpp

clean:
	rm -rf test_default test_small test_containers *.dSYM

.PHONY: test clean
