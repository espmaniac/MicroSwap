#pragma once

/**
 * @file MicroSwap.h
 * @brief Virtual memory with a swap file and STL-like containers for Arduino.
 *
 * Data lives in fixed-size virtual pages. Only a few of them are kept in RAM
 * at a time ("resident pages"); the others are stored in a swap file and are
 * loaded back on access, evicting the least recently used resident page.
 *
 * Containers built on top of it:
 *  - VMVector<T>  vector (one small-heap block while small, whole pages when large)
 *  - VMArray<T,N> fixed-size array
 *  - VMString     mutable string
 *  - VMPtr<T>     pointer to one object, created with make_vm<T>(...)
 *
 * Portability: needs only a C++11 compiler and the C standard library, so it
 * builds for AVR, ESP32, ESP8266, STM32, SAMD, RP2040 and desktop machines.
 * It does not use exceptions or RTTI. The STL is used only when available,
 * to make the iterators work with <algorithm>. RAM for resident pages is
 * allocated statically; malloc is used only for VMVector's chunk table.
 *
 * Reference lifetime: references and pointers returned by the containers
 * point into a resident page. They stay valid until other pages are loaded
 * and evict it; at least (VM_RESIDENT_PAGES - 1) other pages can be touched
 * safely. Use a reference right away; do not keep it while doing many other
 * accesses.
 *
 * Not thread-safe.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Placement new. AVR cores ship <new> since 1.8.3 and <new.h> before that.
#if defined(__has_include)
#  if __has_include(<new>)
#    include <new>
#  else
#    include <new.h>
#  endif
#else
#  include <new>
#endif

// -----------------------------------------------------------------------------
// Configuration
//
// Override any of these with build flags, or with #define before including
// MicroSwap.h. Every file that includes the library must see the same values.
// -----------------------------------------------------------------------------

#if defined(__AVR__)
#  define VM_DEFAULT_PAGE_SIZE      128
#  define VM_DEFAULT_RESIDENT_PAGES 3
#  define VM_DEFAULT_PAGE_COUNT     64
#  define VM_DEFAULT_HEAP_ALIGN     2
#elif defined(ESP32)
#  define VM_DEFAULT_PAGE_SIZE      4096
#  define VM_DEFAULT_RESIDENT_PAGES 8
#  define VM_DEFAULT_PAGE_COUNT     256
#  define VM_DEFAULT_HEAP_ALIGN     8
#else
#  define VM_DEFAULT_PAGE_SIZE      1024
#  define VM_DEFAULT_RESIDENT_PAGES 4
#  define VM_DEFAULT_PAGE_COUNT     128
#  define VM_DEFAULT_HEAP_ALIGN     8
#endif

#ifndef VM_PAGE_SIZE
/// Bytes per page. Also the largest VMString / VMArray / VMPtr object.
#define VM_PAGE_SIZE VM_DEFAULT_PAGE_SIZE
#endif

#ifndef VM_RESIDENT_PAGES
/// Pages kept in RAM at once. RAM used for data = VM_PAGE_SIZE * VM_RESIDENT_PAGES.
#define VM_RESIDENT_PAGES VM_DEFAULT_RESIDENT_PAGES
#endif

#ifndef VM_PAGE_COUNT
/// Total virtual pages. Swap file size = VM_PAGE_SIZE * VM_PAGE_COUNT.
#define VM_PAGE_COUNT VM_DEFAULT_PAGE_COUNT
#endif

#ifndef VM_HEAP_ALIGN
/// Alignment of objects stored in pages.
#define VM_HEAP_ALIGN VM_DEFAULT_HEAP_ALIGN
#endif

#ifndef VM_TICK_LIMIT
/// Testing only: value at which the LRU access counter is renumbered.
#define VM_TICK_LIMIT 0xFFFFFFFFUL
#endif

// Use the STL when it is available (not on AVR) so that iterators work with <algorithm>.
#ifndef VM_USE_STL
#  if defined(__has_include)
#    if __has_include(<type_traits>) && __has_include(<iterator>) && __has_include(<initializer_list>)
#      define VM_USE_STL 1
#    endif
#  endif
#endif
#ifndef VM_USE_STL
#  define VM_USE_STL 0
#endif

#if VM_USE_STL
#  include <initializer_list>
#  include <iterator>
#  include <type_traits>
#endif

static_assert(VM_HEAP_ALIGN >= 2 && (VM_HEAP_ALIGN & (VM_HEAP_ALIGN - 1)) == 0,
              "VM_HEAP_ALIGN must be a power of two, at least 2");
static_assert(VM_PAGE_SIZE % VM_HEAP_ALIGN == 0, "VM_PAGE_SIZE must be a multiple of VM_HEAP_ALIGN");
static_assert(VM_PAGE_SIZE >= 32 && VM_PAGE_SIZE <= 32768, "VM_PAGE_SIZE must be in [32, 32768]");
static_assert(VM_RESIDENT_PAGES >= 3 && VM_RESIDENT_PAGES <= 254,
              "VM_RESIDENT_PAGES must be in [3, 254]; containers touch up to 3 pages at once");
static_assert(VM_PAGE_COUNT >= 1 && VM_PAGE_COUNT < 65535, "VM_PAGE_COUNT must be in [1, 65534]");

// -----------------------------------------------------------------------------
// Small replacements for the STL pieces the library needs
// -----------------------------------------------------------------------------

namespace vm_detail {

template<typename T> struct remove_reference      { typedef T type; };
template<typename T> struct remove_reference<T&>  { typedef T type; };
template<typename T> struct remove_reference<T&&> { typedef T type; };

template<typename T>
constexpr T&& forward(typename remove_reference<T>::type& t) noexcept { return static_cast<T&&>(t); }

template<typename T>
constexpr T&& forward(typename remove_reference<T>::type&& t) noexcept { return static_cast<T&&>(t); }

template<typename T>
constexpr typename remove_reference<T>::type&& move(T&& t) noexcept {
    return static_cast<typename remove_reference<T>::type&&>(t);
}

template<typename T>
void swap(T& a, T& b) {
    T tmp(vm_detail::move(a));
    a = vm_detail::move(b);
    b = vm_detail::move(tmp);
}

template<typename T>
constexpr T min_of(T a, T b) { return b < a ? b : a; }

template<typename T>
constexpr T max_of(T a, T b) { return a < b ? b : a; }

template<bool Cond, typename IfTrue, typename IfFalse>
struct conditional { typedef IfTrue type; };
template<typename IfTrue, typename IfFalse>
struct conditional<false, IfTrue, IfFalse> { typedef IfFalse type; };

template<typename T>
struct is_trivially_destructible {
#if VM_USE_STL
    static const bool value = std::is_trivially_destructible<T>::value;
#elif defined(__clang__)
    static const bool value = __is_trivially_destructible(T);
#else
    static const bool value = __has_trivial_destructor(T);
#endif
};

constexpr size_t align_up(size_t v) { return (v + (VM_HEAP_ALIGN - 1)) & ~size_t(VM_HEAP_ALIGN - 1); }

/// Run the destructor unless it is trivial.
template<typename T>
void destroy(T* p) {
    if (!is_trivially_destructible<T>::value) p->~T();
}

/**
 * @brief Object returned by element access after an error, so callers never
 *        dereference a null pointer. Its contents are meaningless.
 */
template<typename T>
T& placeholder() {
    alignas(T) static unsigned char storage[sizeof(T)];
    return *reinterpret_cast<T*>(storage);
}

/// How a caller is going to use a page.
enum class Access : uint8_t {
    Read,     ///< Only reads.
    MayWrite, ///< Hands a reference to user code, which may or may not write through it.
    Write,    ///< Writes.
};

/// Header in front of every block of a heap page.
struct BlockHeader {
    uint16_t size;    ///< Payload size in bytes (multiple of VM_HEAP_ALIGN).
    uint16_t is_free; ///< 1 if the block is free.
};

enum : uint16_t {
    kNoPage = 0xFFFF,
    kBlockHeaderSize = align_up(sizeof(BlockHeader)),
    kHeapMaxPayload = VM_PAGE_SIZE - kBlockHeaderSize, ///< Largest object in one heap block.
};

enum : uint8_t { kNoFrame = 0xFF };

} // namespace vm_detail

// -----------------------------------------------------------------------------
// Errors
// -----------------------------------------------------------------------------

/// Error codes passed to the error handler and returned by VMManager::last_error().
enum class VMError : uint8_t {
    None,
    NotStarted,  ///< VMManager::begin() has not been called.
    OutOfPages,  ///< Every virtual page is in use.
    TooLarge,    ///< The object does not fit in one page.
    OutOfMemory, ///< malloc failed (VMVector chunk table).
    StorageIO,   ///< Reading or writing the swap file failed.
    OutOfRange,  ///< Index out of range (at(), front(), pop_back() on empty, ...).
    NullPointer, ///< A null VMPtr was dereferenced.
};

/// Human-readable name of an error code.
inline const char* vm_error_name(VMError error) {
    switch (error) {
        case VMError::None:        return "None";
        case VMError::NotStarted:  return "NotStarted";
        case VMError::OutOfPages:  return "OutOfPages";
        case VMError::TooLarge:    return "TooLarge";
        case VMError::OutOfMemory: return "OutOfMemory";
        case VMError::StorageIO:   return "StorageIO";
        case VMError::OutOfRange:  return "OutOfRange";
        case VMError::NullPointer: return "NullPointer";
    }
    return "Unknown";
}

/**
 * @brief Called on every error.
 *
 * The default handler calls abort(). A custom handler may log and reset the
 * board, throw (where exceptions are enabled), or return. If it returns, the
 * failed operation does nothing and element access returns a placeholder
 * object with meaningless contents.
 */
typedef void (*VMErrorHandler)(VMError error);

inline void vm_default_error_handler(VMError) { abort(); }

// -----------------------------------------------------------------------------
// Swap storage
// -----------------------------------------------------------------------------

/**
 * @brief Where swapped-out pages are stored.
 *
 * Implement it to keep pages anywhere (external SPI RAM, EEPROM, ...), or use
 * VMFileStorage for a file.
 */
class VMStorage {
public:
    virtual ~VMStorage() {}
    virtual bool read(uint32_t offset, uint8_t* buf, size_t len) = 0;
    virtual bool write(uint32_t offset, const uint8_t* buf, size_t len) = 0;
    /// Called by VMManager::flush_all() after writing pages.
    virtual bool flush() { return true; }
};

/**
 * @brief Swap storage on any Arduino file with seek(), read(), write() and flush():
 *        SD, SdFat, LittleFS, SPIFFS, ...
 *
 * The file must be opened for both reading and writing, and must stay open
 * while the VMManager uses it:
 *  - ESP32 / ESP8266 (FS.h):  SD.open(path, "w+"), LittleFS.open(path, "w+")
 *  - Arduino SD library:      SD.open(path, O_READ | O_WRITE | O_CREAT)
 *  - SdFat:                   sd.open(path, O_RDWR | O_CREAT | O_TRUNC)
 *
 * Do not use FILE_WRITE with the Arduino SD library: it appends every write
 * to the end of the file.
 */
template<typename FileT>
class VMFileStorage : public VMStorage {
public:
    explicit VMFileStorage(FileT& file) : _file(file) {}

    bool read(uint32_t offset, uint8_t* buf, size_t len) override {
        if (!_file.seek(offset)) return false;
        return static_cast<size_t>(_file.read(buf, len)) == len;
    }

    bool write(uint32_t offset, const uint8_t* buf, size_t len) override {
        if (!_file.seek(offset)) return false;
        return static_cast<size_t>(_file.write(buf, len)) == len;
    }

    bool flush() override {
        _file.flush();
        return true;
    }

private:
    FileT& _file;
};

// Forward declarations for friend declarations
template<typename T> class VMPtr;
template<typename T> class VMVector;
template<typename T, size_t N> class VMArray;
class VMString;

// -----------------------------------------------------------------------------
// VMManager
// -----------------------------------------------------------------------------

/**
 * @brief Singleton that owns the virtual pages, the resident pages in RAM
 *        and the swap storage.
 *
 * Pages are either whole (used by large VMVectors) or "heap pages" split into
 * blocks that hold small objects (VMString, VMArray, VMPtr, small VMVectors).
 */
class VMManager {
public:
    static VMManager& instance() {
        static VMManager inst;
        return inst;
    }

    /**
     * @brief Start using the given swap storage. Discards all previous data.
     * @param storage Storage for swapped-out pages; must outlive the manager's use.
     * @return True on success.
     */
    bool begin(VMStorage& storage) {
        _storage = &storage;
        reset_tables();
        _started = true;
        return true;
    }

    /// Stop using the storage and discard all pages (nothing is written).
    void end() {
        reset_tables();
        _storage = nullptr;
        _started = false;
    }

    /**
     * @brief Write every modified resident page to the swap storage and flush it.
     * @return True if every write succeeded.
     */
    bool flush_all() {
        if (!_started) return false;
        bool ok = true;
        for (uint8_t f = 0; f < VM_RESIDENT_PAGES; ++f)
            if (_frames[f].page != vm_detail::kNoPage) ok = save_frame(f) && ok;
        return _storage->flush() && ok;
    }

    /// Last error since begin() or clear_error().
    VMError last_error() const { return _last_error; }
    void clear_error() { _last_error = VMError::None; }

    /// Set the error handler (nullptr restores the default, which calls abort()).
    void set_error_handler(VMErrorHandler handler) {
        _handler = handler ? handler : vm_default_error_handler;
    }

    size_t get_page_size() const { return VM_PAGE_SIZE; }
    size_t get_page_count() const { return VM_PAGE_COUNT; }
    size_t get_resident_pages() const { return VM_RESIDENT_PAGES; }

    /// Number of virtual pages not in use.
    size_t free_pages() const {
        size_t count = 0;
        for (uint16_t p = 0; p < VM_PAGE_COUNT; ++p)
            if (!(_pages[p].flags & PAGE_ALLOCATED)) count++;
        return count;
    }

    /// Pages loaded from storage since begin().
    uint32_t swap_in_count() const { return _swap_ins; }
    /// Pages written to storage since begin().
    uint32_t swap_out_count() const { return _swap_outs; }

private:
    template<typename T> friend class ::VMPtr;
    template<typename T> friend class ::VMVector;
    template<typename T, size_t N> friend class ::VMArray;
    friend class ::VMString;
    template<typename T, typename... Args> friend VMPtr<T> make_vm(Args&&... args);

    enum : uint8_t {
        PAGE_ALLOCATED = 1,
        PAGE_HEAP      = 2,
    };

    /// Per virtual page bookkeeping (kept in RAM).
    struct PageEntry {
        uint8_t  flags;        ///< PAGE_* bits.
        uint8_t  frame;        ///< Resident page holding it, or kNoFrame.
        uint16_t heap_largest; ///< Heap pages: largest free block, so full pages are skipped without loading them.
    };

    /// A resident page slot in RAM.
    struct Frame {
        uint16_t          page;     ///< Virtual page stored here, or kNoPage.
        vm_detail::Access written;  ///< Strongest access since the page was loaded or saved.
        uint32_t          last_use; ///< Access tick, for least-recently-used eviction.
    };

    VMStorage*     _storage = nullptr;
    bool           _started = false;
    VMError        _last_error = VMError::None;
    VMErrorHandler _handler = vm_default_error_handler;

    uint32_t _tick = 0;
    uint32_t _storage_size = 0; ///< Bytes known to exist in the storage.
    uint32_t _swap_ins = 0;
    uint32_t _swap_outs = 0;

    PageEntry _pages[VM_PAGE_COUNT];
    Frame     _frames[VM_RESIDENT_PAGES];
    alignas(VM_HEAP_ALIGN) uint8_t _frame_data[VM_RESIDENT_PAGES][VM_PAGE_SIZE];

    VMManager() { reset_tables(); }
    VMManager(const VMManager&) = delete;
    VMManager& operator=(const VMManager&) = delete;

    void fail(VMError error) {
        _last_error = error;
        _handler(error);
    }

    void reset_tables() {
        for (uint16_t p = 0; p < VM_PAGE_COUNT; ++p) {
            _pages[p].flags = 0;
            _pages[p].frame = vm_detail::kNoFrame;
            _pages[p].heap_largest = 0;
        }
        for (uint8_t f = 0; f < VM_RESIDENT_PAGES; ++f) {
            _frames[f].page = vm_detail::kNoPage;
            _frames[f].written = vm_detail::Access::Read;
            _frames[f].last_use = 0;
        }
        _tick = 0;
        _storage_size = 0;
        _swap_ins = 0;
        _swap_outs = 0;
        _last_error = VMError::None;
    }

    bool is_allocated(uint16_t page) const {
        return page < VM_PAGE_COUNT && (_pages[page].flags & PAGE_ALLOCATED);
    }

    static uint32_t storage_offset(uint16_t page) { return uint32_t(page) * VM_PAGE_SIZE; }

    // -------------------- Pages --------------------

    /**
     * @brief Allocate a zero-filled page.
     * @return Page index, or kNoPage on failure.
     */
    uint16_t alloc_page() {
        if (!_started) { fail(VMError::NotStarted); return vm_detail::kNoPage; }

        for (uint16_t p = 0; p < VM_PAGE_COUNT; ++p) {
            if (_pages[p].flags & PAGE_ALLOCATED) continue;

            uint8_t f = acquire_frame();
            memset(_frame_data[f], 0, VM_PAGE_SIZE);
            attach(p, f);
            _frames[f].written = vm_detail::Access::Write; // the swap file holds stale data for this page
            _pages[p].flags = PAGE_ALLOCATED;
            _pages[p].heap_largest = 0;
            return p;
        }
        fail(VMError::OutOfPages);
        return vm_detail::kNoPage;
    }

    /// Release a page. Its contents are discarded, not written to storage.
    void free_page(uint16_t page) {
        if (!is_allocated(page)) return;
        uint8_t f = _pages[page].frame;
        if (f != vm_detail::kNoFrame) {
            _frames[f].page = vm_detail::kNoPage;
            _frames[f].written = vm_detail::Access::Read;
        }
        _pages[page].flags = 0;
        _pages[page].frame = vm_detail::kNoFrame;
        _pages[page].heap_largest = 0;
    }

    /**
     * @brief Pointer to the RAM copy of a page, loading it if needed.
     * @param page Allocated page index.
     * @param access How the caller is going to use the page.
     * @return Pointer to VM_PAGE_SIZE bytes, or nullptr if the page is not allocated.
     */
    uint8_t* page_ptr(uint16_t page, vm_detail::Access access) {
        if (!is_allocated(page)) return nullptr;
        if (_pages[page].frame == vm_detail::kNoFrame) load_page(page);

        Frame& frame = _frames[_pages[page].frame];
        frame.last_use = next_tick();
        if (access > frame.written) frame.written = access;
        return _frame_data[_pages[page].frame];
    }

    /// Pointer to a byte offset inside a page (see page_ptr()).
    void* block_ptr(uint16_t page, uint16_t offset, vm_detail::Access access) {
        uint8_t* data = page_ptr(page, access);
        return data ? data + offset : nullptr;
    }

    void attach(uint16_t page, uint8_t f) {
        _frames[f].page = page;
        _frames[f].written = vm_detail::Access::Read;
        _frames[f].last_use = next_tick();
        _pages[page].frame = f;
    }

    /**
     * @brief Next access tick for LRU ordering.
     *
     * When the counter is about to overflow, resident pages are renumbered
     * 1..VM_RESIDENT_PAGES in their current order. Without this, pages used
     * right after the overflow would look the oldest and be evicted first,
     * invalidating references that should still be valid.
     */
    uint32_t next_tick() {
        if (_tick >= VM_TICK_LIMIT) renumber_frames();
        return ++_tick;
    }

    void renumber_frames() {
        uint8_t rank[VM_RESIDENT_PAGES];
        for (uint8_t f = 0; f < VM_RESIDENT_PAGES; ++f) {
            rank[f] = 0;
            for (uint8_t g = 0; g < VM_RESIDENT_PAGES; ++g) {
                const bool older = _frames[g].last_use < _frames[f].last_use ||
                                   (_frames[g].last_use == _frames[f].last_use && g < f);
                if (older) rank[f]++;
            }
        }
        for (uint8_t f = 0; f < VM_RESIDENT_PAGES; ++f) _frames[f].last_use = rank[f] + 1;
        _tick = VM_RESIDENT_PAGES;
    }

    void load_page(uint16_t page) {
        uint8_t f = acquire_frame();
        if (!_storage->read(storage_offset(page), _frame_data[f], VM_PAGE_SIZE)) {
            fail(VMError::StorageIO);
            memset(_frame_data[f], 0, VM_PAGE_SIZE);
        }
        _swap_ins++;
        attach(page, f);
    }

    /// Return a free resident slot, evicting the least recently used page if necessary.
    uint8_t acquire_frame() {
        uint8_t victim = 0;
        for (uint8_t f = 0; f < VM_RESIDENT_PAGES; ++f) {
            if (_frames[f].page == vm_detail::kNoPage) return f;
            if (_frames[f].last_use < _frames[victim].last_use) victim = f;
        }

        save_frame(victim);
        _pages[_frames[victim].page].frame = vm_detail::kNoFrame;
        _frames[victim].page = vm_detail::kNoPage;
        _frames[victim].written = vm_detail::Access::Read;
        return victim;
    }

    /**
     * @brief Write a resident page to storage if it changed.
     *
     * A page handed out only as references (Access::MayWrite) is compared
     * with its stored copy first, so pages that were only read through
     * non-const references are not written again.
     */
    bool save_frame(uint8_t f) {
        const vm_detail::Access written = _frames[f].written;
        if (written == vm_detail::Access::Read) return true;
        if (written == vm_detail::Access::MayWrite && same_as_storage(f)) {
            _frames[f].written = vm_detail::Access::Read;
            return true;
        }

        const uint32_t offset = storage_offset(_frames[f].page);
        bool ok = extend_storage(offset) && _storage->write(offset, _frame_data[f], VM_PAGE_SIZE);
        if (!ok) { fail(VMError::StorageIO); return false; }

        _frames[f].written = vm_detail::Access::Read;
        _storage_size = vm_detail::max_of<uint32_t>(_storage_size, offset + VM_PAGE_SIZE);
        _swap_outs++;
        return true;
    }

    /// True if the resident copy of a page equals the copy in storage.
    bool same_as_storage(uint8_t f) {
        uint8_t chunk[32];
        const uint32_t offset = storage_offset(_frames[f].page);
        for (size_t pos = 0; pos < VM_PAGE_SIZE; pos += sizeof(chunk)) {
            const size_t n = vm_detail::min_of(sizeof(chunk), size_t(VM_PAGE_SIZE - pos));
            if (!_storage->read(offset + pos, chunk, n)) return false;
            if (memcmp(chunk, _frame_data[f] + pos, n) != 0) return false;
        }
        return true;
    }

    /// Fill the storage with zeros up to `size`, since many file systems cannot seek past the end.
    bool extend_storage(uint32_t size) {
        uint8_t zeros[32];
        memset(zeros, 0, sizeof(zeros));
        while (_storage_size < size) {
            size_t n = (size_t)vm_detail::min_of<uint32_t>(sizeof(zeros), size - _storage_size);
            if (!_storage->write(_storage_size, zeros, n)) return false;
            _storage_size += n;
        }
        return true;
    }

    // -------------------- Small-block heap --------------------
    //
    // A heap page is a sequence of blocks that fill it exactly; each block is
    // a BlockHeader followed by its payload. The page can therefore be walked
    // from block to block by size, and no free list is needed.

    static vm_detail::BlockHeader* block_at(uint8_t* data, size_t offset) {
        return reinterpret_cast<vm_detail::BlockHeader*>(data + offset);
    }

    /**
     * @brief Allocate a block of at least `size` bytes.
     * @param[out] out_page   Page holding the block.
     * @param[out] out_offset Offset of the payload inside the page.
     * @param[out] out_usable Usable payload size (>= size).
     * @return True on success.
     */
    bool heap_alloc(size_t size, uint16_t& out_page, uint16_t& out_offset, size_t& out_usable) {
        const size_t need = vm_detail::align_up(size == 0 ? 1 : size);
        if (need > vm_detail::kHeapMaxPayload) { fail(VMError::TooLarge); return false; }

        for (uint16_t p = 0; p < VM_PAGE_COUNT; ++p) {
            if (!(_pages[p].flags & PAGE_HEAP) || _pages[p].heap_largest < need) continue;
            if (alloc_in_heap_page(p, need, out_offset, out_usable)) {
                out_page = p;
                return true;
            }
        }

        uint16_t p = alloc_page();
        if (p == vm_detail::kNoPage) return false;
        _pages[p].flags |= PAGE_HEAP;

        vm_detail::BlockHeader* first = block_at(page_ptr(p, vm_detail::Access::Write), 0);
        first->size = vm_detail::kHeapMaxPayload;
        first->is_free = 1;

        alloc_in_heap_page(p, need, out_offset, out_usable);
        out_page = p;
        return true;
    }

    /// First-fit allocation inside one heap page.
    bool alloc_in_heap_page(uint16_t page, size_t need, uint16_t& out_offset, size_t& out_usable) {
        uint8_t* data = page_ptr(page, vm_detail::Access::Write);
        const size_t header = vm_detail::kBlockHeaderSize;

        for (size_t off = 0; off < VM_PAGE_SIZE; off += header + block_at(data, off)->size) {
            vm_detail::BlockHeader* block = block_at(data, off);
            if (!block->is_free || block->size < need) continue;

            // Split off the remainder if it can hold another block.
            if (block->size >= need + header + VM_HEAP_ALIGN) {
                vm_detail::BlockHeader* rest = block_at(data, off + header + need);
                rest->size = uint16_t(block->size - need - header);
                rest->is_free = 1;
                block->size = uint16_t(need);
            }
            block->is_free = 0;

            out_offset = uint16_t(off + header);
            out_usable = block->size;
            _pages[page].heap_largest = merge_free_blocks(data);
            return true;
        }
        return false;
    }

    /// Free a block; the page is released once it holds no blocks.
    void heap_free(uint16_t page, uint16_t payload_offset) {
        if (!is_allocated(page) || !(_pages[page].flags & PAGE_HEAP)) return;

        uint8_t* data = page_ptr(page, vm_detail::Access::Write);
        block_at(data, payload_offset - vm_detail::kBlockHeaderSize)->is_free = 1;

        const uint16_t largest = merge_free_blocks(data);
        if (largest == vm_detail::kHeapMaxPayload) free_page(page);
        else _pages[page].heap_largest = largest;
    }

    /**
     * @brief Merge adjacent free blocks.
     * @return Size of the largest free block afterwards.
     */
    static uint16_t merge_free_blocks(uint8_t* data) {
        const size_t header = vm_detail::kBlockHeaderSize;
        uint16_t largest = 0;

        for (size_t off = 0; off < VM_PAGE_SIZE; off += header + block_at(data, off)->size) {
            vm_detail::BlockHeader* block = block_at(data, off);
            if (!block->is_free) continue;

            size_t next = off + header + block->size;
            while (next < VM_PAGE_SIZE && block_at(data, next)->is_free) {
                block->size = uint16_t(block->size + header + block_at(data, next)->size);
                next = off + header + block->size;
            }
            largest = vm_detail::max_of(largest, block->size);
        }
        return largest;
    }
};

// -----------------------------------------------------------------------------
// VMPtr
// -----------------------------------------------------------------------------

/**
 * @brief Pointer to one object stored in virtual memory. Create it with make_vm<T>(...).
 *
 * Behaves like a raw pointer: copies point to the same object, and the object
 * lives until destroy() is called on one of them.
 */
template<typename T>
class VMPtr {
public:
    /// Null pointer.
    VMPtr() : _page(vm_detail::kNoPage), _offset(0) {}

    /// True if the pointer refers to an object.
    bool valid() const { return _page != vm_detail::kNoPage; }
    explicit operator bool() const { return valid(); }

    T& operator*() { return *get(); }
    const T& operator*() const { return *get(); }
    T* operator->() { return get(); }
    const T* operator->() const { return get(); }

    /// Pointer to the object in RAM; valid until its page is evicted.
    T* get() { return pointer(vm_detail::Access::MayWrite); }
    const T* get() const { return pointer(vm_detail::Access::Read); }

    /// Destroy the object and free its storage. The pointer becomes null.
    void destroy() {
        if (!valid()) return;
        vm_detail::destroy(pointer(vm_detail::Access::Write));
        VMManager::instance().heap_free(_page, _offset);
        _page = vm_detail::kNoPage;
        _offset = 0;
    }

    bool operator==(const VMPtr& other) const { return _page == other._page && _offset == other._offset; }
    bool operator!=(const VMPtr& other) const { return !(*this == other); }

private:
    template<typename U, typename... Args> friend VMPtr<U> make_vm(Args&&... args);

    VMPtr(uint16_t page, uint16_t offset) : _page(page), _offset(offset) {}

    T* pointer(vm_detail::Access access) const {
        if (!valid()) {
            VMManager::instance().fail(VMError::NullPointer);
            return &vm_detail::placeholder<T>();
        }
        return static_cast<T*>(VMManager::instance().block_ptr(_page, _offset, access));
    }

    uint16_t _page;
    uint16_t _offset;
};

/**
 * @brief Construct a T in virtual memory, like std::make_unique.
 * @return Pointer to the new object, or a null VMPtr if allocation failed.
 *
 * @code
 * VMPtr<Person> p = make_vm<Person>(28, "oscar");
 * p->age_up();
 * p.destroy();
 * @endcode
 */
template<typename T, typename... Args>
VMPtr<T> make_vm(Args&&... args) {
    static_assert(sizeof(T) <= vm_detail::kHeapMaxPayload, "make_vm: object does not fit in one page");
    static_assert(alignof(T) <= VM_HEAP_ALIGN, "make_vm: alignment exceeds VM_HEAP_ALIGN");

    VMManager& mgr = VMManager::instance();
    uint16_t page = vm_detail::kNoPage;
    uint16_t offset = 0;
    size_t usable = 0;
    if (!mgr.heap_alloc(sizeof(T), page, offset, usable)) return VMPtr<T>();

    new (mgr.block_ptr(page, offset, vm_detail::Access::Write)) T(vm_detail::forward<Args>(args)...);
    return VMPtr<T>(page, offset);
}

// -----------------------------------------------------------------------------
// Iterators
// -----------------------------------------------------------------------------

namespace vm_detail {

/// Random-access iterator that accesses elements through Container::operator[].
template<typename Container, typename ValueType, bool Const>
class IndexIterator {
public:
#if VM_USE_STL
    using iterator_category = std::random_access_iterator_tag;
#endif
    using value_type      = ValueType;
    using difference_type = ptrdiff_t;
    using reference       = typename conditional<Const, const ValueType&, ValueType&>::type;
    using pointer         = typename conditional<Const, const ValueType*, ValueType*>::type;
    using ContainerPtr    = typename conditional<Const, const Container*, Container*>::type;

    IndexIterator() : _c(nullptr), _pos(0) {}
    IndexIterator(ContainerPtr c, size_t pos) : _c(c), _pos(pos) {}

    /// Conversion from iterator to const_iterator.
    template<bool OtherConst>
    IndexIterator(const IndexIterator<Container, ValueType, OtherConst>& other) : _c(other._c), _pos(other._pos) {
        static_assert(Const || !OtherConst, "cannot convert const_iterator to iterator");
    }

    reference operator*() const { return (*_c)[_pos]; }
    pointer operator->() const { return &(*_c)[_pos]; }
    reference operator[](difference_type n) const { return (*_c)[_pos + n]; }

    IndexIterator& operator++() { ++_pos; return *this; }
    IndexIterator operator++(int) { IndexIterator tmp = *this; ++_pos; return tmp; }
    IndexIterator& operator--() { --_pos; return *this; }
    IndexIterator operator--(int) { IndexIterator tmp = *this; --_pos; return tmp; }
    IndexIterator& operator+=(difference_type n) { _pos += n; return *this; }
    IndexIterator& operator-=(difference_type n) { _pos -= n; return *this; }
    IndexIterator operator+(difference_type n) const { return IndexIterator(_c, _pos + n); }
    IndexIterator operator-(difference_type n) const { return IndexIterator(_c, _pos - n); }
    friend IndexIterator operator+(difference_type n, const IndexIterator& it) { return it + n; }

    difference_type operator-(const IndexIterator& other) const {
        return difference_type(_pos) - difference_type(other._pos);
    }

    bool operator==(const IndexIterator& other) const { return _c == other._c && _pos == other._pos; }
    bool operator!=(const IndexIterator& other) const { return !(*this == other); }
    bool operator<(const IndexIterator& other) const { return _pos < other._pos; }
    bool operator>(const IndexIterator& other) const { return _pos > other._pos; }
    bool operator<=(const IndexIterator& other) const { return _pos <= other._pos; }
    bool operator>=(const IndexIterator& other) const { return _pos >= other._pos; }

    /// Index of the element in the container.
    size_t pos() const { return _pos; }

private:
    template<typename, typename, bool> friend class IndexIterator;

    ContainerPtr _c;
    size_t _pos;
};

/// Reverse iterator adapter.
template<typename ForwardIter>
class ReverseIterator {
public:
#if VM_USE_STL
    using iterator_category = std::random_access_iterator_tag;
#endif
    using value_type      = typename ForwardIter::value_type;
    using difference_type = typename ForwardIter::difference_type;
    using reference       = typename ForwardIter::reference;
    using pointer         = typename ForwardIter::pointer;

    ReverseIterator() {}
    explicit ReverseIterator(ForwardIter it) : _base(it) {}

    ForwardIter base() const { return _base; }

    reference operator*() const { ForwardIter tmp = _base; --tmp; return *tmp; }
    pointer operator->() const { return &(**this); }
    reference operator[](difference_type n) const { return *(*this + n); }

    ReverseIterator& operator++() { --_base; return *this; }
    ReverseIterator operator++(int) { ReverseIterator tmp = *this; --_base; return tmp; }
    ReverseIterator& operator--() { ++_base; return *this; }
    ReverseIterator operator--(int) { ReverseIterator tmp = *this; ++_base; return tmp; }
    ReverseIterator& operator+=(difference_type n) { _base -= n; return *this; }
    ReverseIterator& operator-=(difference_type n) { _base += n; return *this; }
    ReverseIterator operator+(difference_type n) const { return ReverseIterator(_base - n); }
    ReverseIterator operator-(difference_type n) const { return ReverseIterator(_base + n); }

    difference_type operator-(const ReverseIterator& other) const { return other._base - _base; }

    bool operator==(const ReverseIterator& other) const { return _base == other._base; }
    bool operator!=(const ReverseIterator& other) const { return _base != other._base; }
    bool operator<(const ReverseIterator& other) const { return other._base < _base; }
    bool operator>(const ReverseIterator& other) const { return other._base > _base; }
    bool operator<=(const ReverseIterator& other) const { return !(*this > other); }
    bool operator>=(const ReverseIterator& other) const { return !(*this < other); }

private:
    ForwardIter _base;
};

} // namespace vm_detail

// -----------------------------------------------------------------------------
// VMVector
// -----------------------------------------------------------------------------

/**
 * @brief Vector stored in virtual memory.
 *
 * While small, the elements share one block of a heap page ("flat mode", data()
 * is available). When they no longer fit in one block, they move to whole
 * pages ("paged mode"); element i then lives in page i / elements-per-page.
 */
template<typename T>
class VMVector {
    static_assert(sizeof(T) <= VM_PAGE_SIZE, "VMVector: element type must fit in one page");
    static_assert(alignof(T) <= VM_HEAP_ALIGN, "VMVector: alignment exceeds VM_HEAP_ALIGN");

    enum : size_t {
        kPerPage = VM_PAGE_SIZE / sizeof(T),                 ///< Elements per page in paged mode.
        kMaxFlat = vm_detail::kHeapMaxPayload / sizeof(T),   ///< Elements that fit in one heap block.
    };

public:
    typedef T value_type;
    typedef T& reference;
    typedef const T& const_reference;
    typedef size_t size_type;
    typedef ptrdiff_t difference_type;

    using iterator               = vm_detail::IndexIterator<VMVector, T, false>;
    using const_iterator         = vm_detail::IndexIterator<VMVector, T, true>;
    using reverse_iterator       = vm_detail::ReverseIterator<iterator>;
    using const_reverse_iterator = vm_detail::ReverseIterator<const_iterator>;

    VMVector() {}
    explicit VMVector(size_type n, const T& value = T()) { assign(n, value); }
#if VM_USE_STL
    VMVector(std::initializer_list<T> init) { assign(init.begin(), init.end()); }
#endif
    VMVector(const VMVector& other) { assign(other.begin(), other.end()); }
    VMVector(VMVector&& other) noexcept { swap(other); }

    VMVector& operator=(const VMVector& other) {
        if (this != &other) assign(other.begin(), other.end());
        return *this;
    }

    VMVector& operator=(VMVector&& other) noexcept {
        if (this != &other) {
            clear();
            swap(other);
        }
        return *this;
    }

    ~VMVector() { clear(); }

    // -------------------- Element access --------------------

    reference operator[](size_type i) { return *element(i, vm_detail::Access::MayWrite); }
    const_reference operator[](size_type i) const { return *element(i, vm_detail::Access::Read); }

    reference at(size_type i) { return in_range(i) ? (*this)[i] : vm_detail::placeholder<T>(); }
    const_reference at(size_type i) const { return in_range(i) ? (*this)[i] : vm_detail::placeholder<T>(); }

    reference front() { return at(0); }
    const_reference front() const { return at(0); }
    reference back() { return at(_size - 1); }
    const_reference back() const { return at(_size - 1); }

    /// Contiguous storage; nullptr in paged mode or when nothing is allocated.
    T* data() { return has_flat_block() ? flat_element(0, vm_detail::Access::MayWrite) : nullptr; }
    const T* data() const { return has_flat_block() ? flat_element(0, vm_detail::Access::Read) : nullptr; }

    // -------------------- Capacity --------------------

    bool empty() const { return _size == 0; }
    size_type size() const { return _size; }
    size_type capacity() const { return _flat_mode ? _flat_capacity : _chunk_count * kPerPage; }

    /// True while the elements are stored in one block (data() is available).
    bool is_flat() const { return _flat_mode; }

    void reserve(size_type n) {
        if (n <= capacity()) return;
        if (_flat_mode && grow_flat_block(n)) return;
        if (_flat_mode && !transition_to_paged()) return;
        ensure_chunks(n);
    }

    /// Release pages beyond those needed for size() (paged mode).
    void shrink_to_fit() {
        if (_flat_mode) return;
        while (_chunk_count > chunks_for(_size)) release_last_chunk();
    }

    // -------------------- Modifiers --------------------

    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(vm_detail::move(value)); }

    /// Construct an element at the end. Returns a placeholder if there is no room.
    template<typename... Args>
    reference emplace_back(Args&&... args) {
        T* slot = back_slot();
        if (!slot) return vm_detail::placeholder<T>();
        new (slot) T(vm_detail::forward<Args>(args)...);
        _size++;
        return *slot;
    }

    /// Construct an element before pos.
    template<typename... Args>
    iterator emplace(const_iterator pos, Args&&... args) {
        const size_type idx = pos.pos();
        const size_type old_size = _size;
        emplace_back(vm_detail::forward<Args>(args)...);
        if (_size == old_size) return end();

        // Bubble the new element down to its position.
        for (size_type i = _size - 1; i > idx; --i)
            vm_detail::swap(*element(i, vm_detail::Access::Write), *element(i - 1, vm_detail::Access::Write));
        return iterator(this, idx);
    }

    iterator insert(const_iterator pos, const T& value) { return emplace(pos, value); }
    iterator insert(const_iterator pos, T&& value) { return emplace(pos, vm_detail::move(value)); }

    iterator erase(const_iterator pos) {
        const size_type idx = pos.pos();
        if (idx >= _size) return end();
        for (size_type i = idx; i + 1 < _size; ++i)
            *element(i, vm_detail::Access::Write) = vm_detail::move(*element(i + 1, vm_detail::Access::Write));
        pop_back();
        return iterator(this, idx);
    }

    void pop_back() {
        if (_size == 0) { VMManager::instance().fail(VMError::OutOfRange); return; }
        _size--;
        if (!vm_detail::is_trivially_destructible<T>::value) element(_size, vm_detail::Access::Write)->~T();

        // Return a page as soon as the last one becomes empty.
        if (!_flat_mode && _chunk_count > chunks_for(_size)) release_last_chunk();
    }

    /// Destroy all elements and free all storage.
    void clear() {
        if (!vm_detail::is_trivially_destructible<T>::value)
            for (size_type i = 0; i < _size; ++i) element(i, vm_detail::Access::Write)->~T();
        _size = 0;

        if (has_flat_block()) VMManager::instance().heap_free(_flat_page, _flat_offset);
        _flat_page = vm_detail::kNoPage;
        _flat_offset = 0;
        _flat_capacity = 0;
        _flat_mode = true;

        while (_chunk_count > 0) release_last_chunk();
        free(_chunks);
        _chunks = nullptr;
        _chunk_slots = 0;
    }

    void resize(size_type n, const T& value = T()) {
        while (_size > n) pop_back();
        while (_size < n) {
            const size_type old_size = _size;
            push_back(value);
            if (_size == old_size) return; // out of memory
        }
    }

    void assign(size_type n, const T& value) {
        clear();
        resize(n, value);
    }

    template<typename InputIt>
    void assign(InputIt first, InputIt last) {
        clear();
        for (; first != last; ++first) push_back(*first);
    }

    void swap(VMVector& other) {
        vm_detail::swap(_size, other._size);
        vm_detail::swap(_flat_mode, other._flat_mode);
        vm_detail::swap(_flat_page, other._flat_page);
        vm_detail::swap(_flat_offset, other._flat_offset);
        vm_detail::swap(_flat_capacity, other._flat_capacity);
        vm_detail::swap(_chunks, other._chunks);
        vm_detail::swap(_chunk_count, other._chunk_count);
        vm_detail::swap(_chunk_slots, other._chunk_slots);
    }

    // -------------------- Iterators --------------------

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, _size); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, _size); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    reverse_iterator rbegin() { return reverse_iterator(end()); }
    reverse_iterator rend() { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const { return rbegin(); }
    const_reverse_iterator crend() const { return rend(); }

    // -------------------- Comparisons --------------------

    bool operator==(const VMVector& other) const {
        if (_size != other._size) return false;
        for (size_type i = 0; i < _size; ++i)
            if (!((*this)[i] == other[i])) return false;
        return true;
    }
    bool operator!=(const VMVector& other) const { return !(*this == other); }

    bool operator<(const VMVector& other) const {
        const size_type n = vm_detail::min_of(_size, other._size);
        for (size_type i = 0; i < n; ++i) {
            if ((*this)[i] < other[i]) return true;
            if (other[i] < (*this)[i]) return false;
        }
        return _size < other._size;
    }
    bool operator>(const VMVector& other) const { return other < *this; }
    bool operator<=(const VMVector& other) const { return !(other < *this); }
    bool operator>=(const VMVector& other) const { return !(*this < other); }

private:
    size_type _size = 0;

    // Flat mode: one heap block.
    bool      _flat_mode = true;
    uint16_t  _flat_page = vm_detail::kNoPage;
    uint16_t  _flat_offset = 0;
    size_type _flat_capacity = 0;

    // Paged mode: one whole page per chunk; the table itself is in RAM.
    uint16_t* _chunks = nullptr;
    uint16_t  _chunk_count = 0;
    uint16_t  _chunk_slots = 0;

    bool in_range(size_type i) const {
        if (i < _size) return true;
        VMManager::instance().fail(VMError::OutOfRange);
        return false;
    }

    bool has_flat_block() const { return _flat_mode && _flat_page != vm_detail::kNoPage; }

    static size_type chunks_for(size_type n) { return (n + kPerPage - 1) / kPerPage; }

    T* element(size_type i, vm_detail::Access access) const {
        return _flat_mode ? flat_element(i, access) : paged_element(i, access);
    }

    T* flat_element(size_type i, vm_detail::Access access) const {
        return static_cast<T*>(VMManager::instance().block_ptr(_flat_page, _flat_offset, access)) + i;
    }

    T* paged_element(size_type i, vm_detail::Access access) const {
        uint8_t* page = VMManager::instance().page_ptr(_chunks[i / kPerPage], access);
        return reinterpret_cast<T*>(page) + i % kPerPage;
    }

    /// Make room for one more element; returns where it goes, or nullptr.
    T* back_slot() {
        if (_flat_mode && _size < _flat_capacity) return flat_element(_size, vm_detail::Access::Write);
        if (_flat_mode && grow_flat_block(_size + 1)) return flat_element(_size, vm_detail::Access::Write);
        if (_flat_mode && !transition_to_paged()) return nullptr;
        if (!ensure_chunks(_size + 1)) return nullptr;
        return paged_element(_size, vm_detail::Access::Write);
    }

    /**
     * @brief Move the elements to a larger heap block holding at least n elements.
     * @return False if n elements cannot fit in one block or allocation failed.
     */
    bool grow_flat_block(size_type n) {
        if (n > kMaxFlat) return false;

        // Grow geometrically so repeated push_back stays cheap.
        size_type new_cap = vm_detail::max_of<size_type>(n, _flat_capacity * 2);
        new_cap = vm_detail::min_of<size_type>(vm_detail::max_of<size_type>(new_cap, 4), kMaxFlat);

        VMManager& mgr = VMManager::instance();
        uint16_t page = vm_detail::kNoPage;
        uint16_t offset = 0;
        size_t usable = 0;
        if (!mgr.heap_alloc(new_cap * sizeof(T), page, offset, usable)) return false;

        for (size_type i = 0; i < _size; ++i) {
            T* src = flat_element(i, vm_detail::Access::Write);
            T* dst = static_cast<T*>(mgr.block_ptr(page, offset, vm_detail::Access::Write)) + i;
            new (dst) T(vm_detail::move(*src));
            vm_detail::destroy(src);
        }

        if (has_flat_block()) mgr.heap_free(_flat_page, _flat_offset);
        _flat_page = page;
        _flat_offset = offset;
        _flat_capacity = usable / sizeof(T);
        return true;
    }

    /// Move the elements from the flat block to whole pages.
    bool transition_to_paged() {
        // Allocate first, so a failure leaves the vector untouched.
        if (!ensure_chunks(_size)) return false;

        for (size_type i = 0; i < _size; ++i) {
            T* src = flat_element(i, vm_detail::Access::Write);
            new (paged_element(i, vm_detail::Access::Write)) T(vm_detail::move(*src));
            vm_detail::destroy(src);
        }

        if (has_flat_block()) VMManager::instance().heap_free(_flat_page, _flat_offset);
        _flat_page = vm_detail::kNoPage;
        _flat_offset = 0;
        _flat_capacity = 0;
        _flat_mode = false;
        return true;
    }

    /// Allocate pages until n elements fit (paged layout).
    bool ensure_chunks(size_type n) {
        while (_chunk_count < chunks_for(n)) {
            if (_chunk_count >= VM_PAGE_COUNT) { VMManager::instance().fail(VMError::OutOfPages); return false; }
            if (_chunk_count == _chunk_slots && !grow_chunk_table()) return false;
            const uint16_t page = VMManager::instance().alloc_page();
            if (page == vm_detail::kNoPage) return false;
            _chunks[_chunk_count++] = page;
        }
        return true;
    }

    bool grow_chunk_table() {
        const uint16_t slots = vm_detail::min_of<uint16_t>(_chunk_slots ? _chunk_slots * 2 : 4, VM_PAGE_COUNT);
        uint16_t* table = static_cast<uint16_t*>(realloc(_chunks, slots * sizeof(uint16_t)));
        if (!table) { VMManager::instance().fail(VMError::OutOfMemory); return false; }
        _chunks = table;
        _chunk_slots = slots;
        return true;
    }

    void release_last_chunk() {
        VMManager::instance().free_page(_chunks[--_chunk_count]);
    }
};

// -----------------------------------------------------------------------------
// VMArray
// -----------------------------------------------------------------------------

/**
 * @brief Fixed-size array stored in one heap block.
 *
 * Elements are value-initialized (zero for int, float, POD structs) and
 * destroyed with the array, like std::array. Storage is allocated on first
 * access, so arrays can be global variables created before VMManager::begin().
 */
template<typename T, size_t N>
class VMArray {
    static_assert(N * sizeof(T) <= vm_detail::kHeapMaxPayload, "VMArray: N * sizeof(T) must fit in one page");
    static_assert(alignof(T) <= VM_HEAP_ALIGN, "VMArray: alignment exceeds VM_HEAP_ALIGN");

public:
    typedef T value_type;
    typedef T& reference;
    typedef const T& const_reference;
    typedef size_t size_type;
    typedef ptrdiff_t difference_type;

    using iterator               = vm_detail::IndexIterator<VMArray, T, false>;
    using const_iterator         = vm_detail::IndexIterator<VMArray, T, true>;
    using reverse_iterator       = vm_detail::ReverseIterator<iterator>;
    using const_reverse_iterator = vm_detail::ReverseIterator<const_iterator>;

    VMArray() {}

    VMArray(const VMArray& other) {
        if (!other.allocated() || !allocate()) return;
        for (size_type i = 0; i < N; ++i) new (raw_element(i)) T(other[i]);
    }

    VMArray(VMArray&& other) noexcept : _page(other._page), _offset(other._offset) {
        other._page = vm_detail::kNoPage;
    }

    VMArray& operator=(const VMArray& other) {
        if (this != &other)
            for (size_type i = 0; i < N; ++i) *element(i, vm_detail::Access::Write) = other[i];
        return *this;
    }

    VMArray& operator=(VMArray&& other) noexcept {
        if (this != &other) {
            release();
            _page = other._page;
            _offset = other._offset;
            other._page = vm_detail::kNoPage;
        }
        return *this;
    }

    ~VMArray() { release(); }

    reference operator[](size_type i) { return *element(i, vm_detail::Access::MayWrite); }
    const_reference operator[](size_type i) const { return *element(i, vm_detail::Access::Read); }

    reference at(size_type i) { return in_range(i) ? (*this)[i] : vm_detail::placeholder<T>(); }
    const_reference at(size_type i) const { return in_range(i) ? (*this)[i] : vm_detail::placeholder<T>(); }

    reference front() { return (*this)[0]; }
    const_reference front() const { return (*this)[0]; }
    reference back() { return (*this)[N - 1]; }
    const_reference back() const { return (*this)[N - 1]; }

    constexpr size_type size() const { return N; }
    constexpr bool empty() const { return N == 0; }

    void fill(const T& value) {
        for (size_type i = 0; i < N; ++i) *element(i, vm_detail::Access::Write) = value;
    }

    /// Assign T() to every element.
    void clear() { fill(T()); }

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, N); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, N); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    reverse_iterator rbegin() { return reverse_iterator(end()); }
    reverse_iterator rend() { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const { return rbegin(); }
    const_reverse_iterator crend() const { return rend(); }

private:
    // Mutable because storage is allocated lazily, also from const accessors.
    mutable uint16_t _page = vm_detail::kNoPage;
    mutable uint16_t _offset = 0;

    bool allocated() const { return _page != vm_detail::kNoPage; }

    /// Allocate the block without constructing elements.
    bool allocate() const {
        size_t usable = 0;
        return VMManager::instance().heap_alloc(N * sizeof(T), _page, _offset, usable);
    }

    T* raw_element(size_type i) const {
        return static_cast<T*>(VMManager::instance().block_ptr(_page, _offset, vm_detail::Access::Write)) + i;
    }

    void release() {
        if (!allocated()) return;
        if (!vm_detail::is_trivially_destructible<T>::value)
            for (size_type i = 0; i < N; ++i) raw_element(i)->~T();
        VMManager::instance().heap_free(_page, _offset);
        _page = vm_detail::kNoPage;
    }

    bool in_range(size_type i) const {
        if (i < N) return true;
        VMManager::instance().fail(VMError::OutOfRange);
        return false;
    }

    T* element(size_type i, vm_detail::Access access) const {
        if (!allocated()) {
            if (!allocate()) return &vm_detail::placeholder<T>();
            for (size_type k = 0; k < N; ++k) new (raw_element(k)) T();
        }
        return static_cast<T*>(VMManager::instance().block_ptr(_page, _offset, access)) + i;
    }
};

// -----------------------------------------------------------------------------
// VMString
// -----------------------------------------------------------------------------

/**
 * @brief Mutable string stored in one heap block.
 *
 * The maximum length is max_size() (one page minus a few bytes). An empty
 * string uses no virtual memory.
 *
 * c_str() and element references point into a resident page; see the note on
 * reference lifetime at the top of this file.
 */
class VMString {
public:
    typedef char value_type;
    typedef char& reference;
    typedef const char& const_reference;
    typedef size_t size_type;
    typedef ptrdiff_t difference_type;

    using iterator               = vm_detail::IndexIterator<VMString, char, false>;
    using const_iterator         = vm_detail::IndexIterator<VMString, char, true>;
    using reverse_iterator       = vm_detail::ReverseIterator<iterator>;
    using const_reverse_iterator = vm_detail::ReverseIterator<const_iterator>;

    static const size_type npos = static_cast<size_type>(-1);

    // -------------------- Construction --------------------

    VMString() {}
    explicit VMString(size_type initial_capacity) { reserve(initial_capacity); }
    VMString(const char* s) { assign(s); }
    VMString(const char* s, size_type count) { assign(s, count); }
    VMString(size_type count, char ch) { assign(count, ch); }
    VMString(const VMString& other) { assign(other.c_str(), other._size); }

    VMString(VMString&& other) noexcept
        : _page(other._page), _offset(other._offset), _size(other._size), _capacity(other._capacity) {
        other.forget_block();
    }

    VMString& operator=(const VMString& other) {
        if (this != &other) assign(other.c_str(), other._size);
        return *this;
    }

    VMString& operator=(VMString&& other) noexcept {
        if (this != &other) {
            release();
            _page = other._page;
            _offset = other._offset;
            _size = other._size;
            _capacity = other._capacity;
            other.forget_block();
        }
        return *this;
    }

    VMString& operator=(const char* s) { assign(s); return *this; }

    ~VMString() { release(); }

    // -------------------- Iterators --------------------

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, _size); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, _size); }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    reverse_iterator rbegin() { return reverse_iterator(end()); }
    reverse_iterator rend() { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const { return rbegin(); }
    const_reverse_iterator crend() const { return rend(); }

    // -------------------- Element access --------------------

    reference operator[](size_type i) { return in_range(i) ? buffer(vm_detail::Access::MayWrite)[i] : vm_detail::placeholder<char>(); }
    const_reference operator[](size_type i) const { return in_range(i) ? buffer(vm_detail::Access::Read)[i] : vm_detail::placeholder<char>(); }
    reference at(size_type i) { return (*this)[i]; }
    const_reference at(size_type i) const { return (*this)[i]; }
    reference front() { return (*this)[0]; }
    const_reference front() const { return (*this)[0]; }
    reference back() { return (*this)[_size - 1]; }
    const_reference back() const { return (*this)[_size - 1]; }

    /// Null-terminated contents; valid until the page is evicted.
    const char* c_str() const { return has_block() ? buffer(vm_detail::Access::Read) : ""; }

    // -------------------- Capacity --------------------

    bool empty() const { return _size == 0; }
    size_type size() const { return _size; }
    size_type length() const { return _size; }
    size_type capacity() const { return _capacity; }

    /// Longest string that fits in one heap block.
    static constexpr size_type max_size() { return vm_detail::kHeapMaxPayload - 1; }

    void reserve(size_type new_cap) { ensure_capacity(new_cap, new_cap); }

    /// No-op: a string always uses a single block.
    void shrink_to_fit() {}

    void resize(size_type n, char ch = '\0') {
        if (n < _size) erase(n);
        else append(n - _size, ch);
    }

    void clear() { erase(); }

    // -------------------- Modifiers --------------------

    void assign(const char* s) { assign(s, strlen(s)); }
    void assign(const char* s, size_type count) { replace(0, _size, s, count); }
    void assign(size_type count, char ch) {
        if (char* gap = open_gap(0, _size, count)) memset(gap, ch, count);
    }
    void assign(const VMString& other, size_type pos, size_type count = npos) {
        if (pos > other._size) { VMManager::instance().fail(VMError::OutOfRange); return; }
        VMString part = other.substr(pos, count);
        assign(part.c_str(), part._size);
    }

    VMString& append(const char* s) { return append(s, strlen(s)); }
    VMString& append(const char* s, size_type count) { return replace(_size, 0, s, count); }
    VMString& append(const VMString& other) { return append(other.c_str(), other._size); }
    VMString& append(size_type count, char ch) { return insert(_size, count, ch); }

    void push_back(char c) { append(1, c); }

    void pop_back() {
        if (empty()) { VMManager::instance().fail(VMError::OutOfRange); return; }
        erase(_size - 1, 1);
    }

    VMString& operator+=(const char* s) { return append(s); }
    VMString& operator+=(const VMString& s) { return append(s); }
    VMString& operator+=(char c) { push_back(c); return *this; }

    VMString& insert(size_type pos, const char* s) { return insert(pos, s, strlen(s)); }
    VMString& insert(size_type pos, const char* s, size_type count) { return replace(pos, 0, s, count); }
    VMString& insert(size_type pos, const VMString& other) { return insert(pos, other.c_str(), other._size); }
    VMString& insert(size_type pos, size_type count, char ch) {
        if (char* gap = open_gap(pos, 0, count)) memset(gap, ch, count);
        return *this;
    }

    VMString& erase(size_type pos = 0, size_type count = npos) {
        open_gap(pos, count, 0);
        return *this;
    }

    /// Replace count characters at pos with s[0, s_count).
    VMString& replace(size_type pos, size_type count, const char* s, size_type s_count) {
        if (points_into_buffer(s)) {
            // s would move or be overwritten while editing; work from a copy.
            VMString copy(s, s_count);
            return replace(pos, count, copy.c_str(), s_count);
        }
        if (char* gap = open_gap(pos, count, s_count)) memcpy(gap, s, s_count);
        return *this;
    }
    VMString& replace(size_type pos, size_type count, const char* s) { return replace(pos, count, s, strlen(s)); }
    VMString& replace(size_type pos, size_type count, const VMString& other) {
        return replace(pos, count, other.c_str(), other._size);
    }

    VMString substr(size_type pos = 0, size_type count = npos) const {
        if (pos > _size) { VMManager::instance().fail(VMError::OutOfRange); return VMString(); }
        return VMString(c_str() + pos, vm_detail::min_of(count, _size - pos));
    }

    /// Copy up to count characters starting at pos to dest (not null-terminated).
    size_type copy(char* dest, size_type count, size_type pos = 0) const {
        if (pos > _size) { VMManager::instance().fail(VMError::OutOfRange); return 0; }
        const size_type n = vm_detail::min_of(count, _size - pos);
        memcpy(dest, c_str() + pos, n);
        return n;
    }

    void swap(VMString& other) {
        vm_detail::swap(_page, other._page);
        vm_detail::swap(_offset, other._offset);
        vm_detail::swap(_size, other._size);
        vm_detail::swap(_capacity, other._capacity);
    }

    // -------------------- Search --------------------

    size_type find(const char* s, size_type pos, size_type count) const {
        if (count == 0) return pos <= _size ? pos : npos;
        const char* b = c_str();
        for (size_type i = pos; i + count <= _size; ++i)
            if (memcmp(b + i, s, count) == 0) return i;
        return npos;
    }
    size_type find(const char* s, size_type pos = 0) const { return find(s, pos, strlen(s)); }
    size_type find(const VMString& other, size_type pos = 0) const { return find(other.c_str(), pos, other._size); }
    size_type find(char ch, size_type pos = 0) const { return find(&ch, pos, 1); }

    size_type rfind(const char* s, size_type pos, size_type count) const {
        if (count > _size) return npos;
        const char* b = c_str();
        for (size_type i = vm_detail::min_of(pos, _size - count) + 1; i-- > 0;)
            if (memcmp(b + i, s, count) == 0) return i;
        return npos;
    }
    size_type rfind(const char* s, size_type pos = npos) const { return rfind(s, pos, strlen(s)); }
    size_type rfind(const VMString& other, size_type pos = npos) const { return rfind(other.c_str(), pos, other._size); }
    size_type rfind(char ch, size_type pos = npos) const { return rfind(&ch, pos, 1); }

    size_type find_first_of(const char* s, size_type pos = 0) const { return find_forward(s, pos, true); }
    size_type find_first_of(const VMString& other, size_type pos = 0) const { return find_first_of(other.c_str(), pos); }
    size_type find_first_of(char ch, size_type pos = 0) const { return find(ch, pos); }

    size_type find_first_not_of(const char* s, size_type pos = 0) const { return find_forward(s, pos, false); }
    size_type find_first_not_of(const VMString& other, size_type pos = 0) const { return find_first_not_of(other.c_str(), pos); }
    size_type find_first_not_of(char ch, size_type pos = 0) const { const char s[2] = {ch, '\0'}; return find_first_not_of(s, pos); }

    size_type find_last_of(const char* s, size_type pos = npos) const { return find_backward(s, pos, true); }
    size_type find_last_of(const VMString& other, size_type pos = npos) const { return find_last_of(other.c_str(), pos); }
    size_type find_last_of(char ch, size_type pos = npos) const { return rfind(ch, pos); }

    size_type find_last_not_of(const char* s, size_type pos = npos) const { return find_backward(s, pos, false); }
    size_type find_last_not_of(const VMString& other, size_type pos = npos) const { return find_last_not_of(other.c_str(), pos); }
    size_type find_last_not_of(char ch, size_type pos = npos) const { const char s[2] = {ch, '\0'}; return find_last_not_of(s, pos); }

    // -------------------- Comparison --------------------

    int compare(const char* s, size_type s_len) const {
        const int r = memcmp(c_str(), s, vm_detail::min_of(_size, s_len));
        if (r != 0) return r;
        return _size == s_len ? 0 : (_size < s_len ? -1 : 1);
    }
    int compare(const char* s) const { return compare(s, strlen(s)); }
    int compare(const VMString& other) const { return compare(other.c_str(), other._size); }

    bool operator==(const VMString& other) const { return compare(other) == 0; }
    bool operator!=(const VMString& other) const { return compare(other) != 0; }
    bool operator<(const VMString& other) const { return compare(other) < 0; }
    bool operator>(const VMString& other) const { return compare(other) > 0; }
    bool operator<=(const VMString& other) const { return compare(other) <= 0; }
    bool operator>=(const VMString& other) const { return compare(other) >= 0; }
    bool operator==(const char* s) const { return compare(s) == 0; }
    bool operator!=(const char* s) const { return compare(s) != 0; }

private:
    uint16_t  _page = vm_detail::kNoPage;
    uint16_t  _offset = 0;
    size_type _size = 0;
    size_type _capacity = 0; ///< Characters that fit, excluding the terminator.

    bool has_block() const { return _page != vm_detail::kNoPage; }

    char* buffer(vm_detail::Access access) const {
        return static_cast<char*>(VMManager::instance().block_ptr(_page, _offset, access));
    }

    bool in_range(size_type i) const {
        if (i < _size) return true;
        VMManager::instance().fail(VMError::OutOfRange);
        return false;
    }

    bool points_into_buffer(const char* s) const {
        if (!has_block()) return false;
        const char* b = buffer(vm_detail::Access::Read);
        return s >= b && s <= b + _capacity;
    }

    void forget_block() {
        _page = vm_detail::kNoPage;
        _offset = 0;
        _size = 0;
        _capacity = 0;
    }

    void release() {
        if (has_block()) VMManager::instance().heap_free(_page, _offset);
        forget_block();
    }

    /**
     * @brief Make sure `needed` characters fit, reallocating to at least `wanted` if not.
     * @return False if the string cannot grow that much.
     */
    bool ensure_capacity(size_type needed, size_type wanted) {
        if (has_block() && needed <= _capacity) return true;
        if (needed > max_size()) { VMManager::instance().fail(VMError::TooLarge); return false; }

        const size_type new_cap = vm_detail::min_of(vm_detail::max_of(wanted, needed), max_size());
        VMManager& mgr = VMManager::instance();
        uint16_t page = vm_detail::kNoPage;
        uint16_t offset = 0;
        size_t usable = 0;
        if (!mgr.heap_alloc(new_cap + 1, page, offset, usable)) return false;

        char* dst = static_cast<char*>(mgr.block_ptr(page, offset, vm_detail::Access::Write));
        if (has_block()) memcpy(dst, buffer(vm_detail::Access::Read), _size + 1);
        else dst[0] = '\0';

        if (has_block()) mgr.heap_free(_page, _offset);
        _page = page;
        _offset = offset;
        _capacity = vm_detail::min_of<size_type>(usable - 1, max_size());
        return true;
    }

    /**
     * @brief Replace `count` characters at `pos` with an uninitialized gap of `gap_len` characters.
     * @return Pointer to the gap (valid until the page is evicted), or nullptr on failure.
     */
    char* open_gap(size_type pos, size_type count, size_type gap_len) {
        if (pos > _size) { VMManager::instance().fail(VMError::OutOfRange); return nullptr; }
        count = vm_detail::min_of(count, _size - pos);

        const size_type new_size = _size - count + gap_len;
        if (new_size == 0 && !has_block()) return nullptr; // stays empty
        if (!ensure_capacity(new_size, _capacity + _capacity / 2)) return nullptr;

        char* b = buffer(vm_detail::Access::Write);
        // Shift the tail, including the terminator.
        memmove(b + pos + gap_len, b + pos + count, _size - pos - count + 1);
        _size = new_size;
        return b + pos;
    }

    size_type find_forward(const char* set, size_type pos, bool in_set) const {
        const char* b = c_str();
        for (size_type i = pos; i < _size; ++i)
            if ((strchr(set, b[i]) != nullptr) == in_set) return i;
        return npos;
    }

    size_type find_backward(const char* set, size_type pos, bool in_set) const {
        const char* b = c_str();
        for (size_type i = vm_detail::min_of(pos, _size - 1) + 1; _size > 0 && i-- > 0;)
            if ((strchr(set, b[i]) != nullptr) == in_set) return i;
        return npos;
    }
};

inline VMString operator+(const VMString& a, const VMString& b) {
    VMString r(a.size() + b.size());
    r.append(a);
    r.append(b);
    return r;
}

inline VMString operator+(const VMString& a, const char* b) {
    VMString r(a.size() + strlen(b));
    r.append(a);
    r.append(b);
    return r;
}

inline VMString operator+(const char* a, const VMString& b) {
    VMString r(strlen(a) + b.size());
    r.append(a);
    r.append(b);
    return r;
}
