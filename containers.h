#pragma once

/**
 * @file containers.h
 * @brief Virtual memory manager, STL-like containers, and VMPtr smart pointer with paging & swapping for Arduino.
 *
 * @details
 * This header provides:
 *  - VMManager: a lightweight virtual memory manager that pages data to/from a swap file on Arduino-compatible filesystems
 *    (e.g., SPIFFS / LittleFS) using a fixed number of RAM-backed pages.
 *  - STL-like containers (VMVector, VMArray, VMString) that transparently use the virtual memory pages as backing storage.
 *  - VMPtr<T>: a smart pointer to objects stored inside a virtual memory page with pointer arithmetic, indexing, and
 *    transparent swap-in on access. Its internal constructor (page, offset) is protected to prevent unsafe user creation;
 *    users should rely on default construction and let pages be allocated lazily on first access.
 *
 * Core features:
 *  - Fixed number of pages (compile-time constants VM_PAGE_SIZE / VM_PAGE_COUNT).
 *  - On-demand page allocation with optional zeroing and reuse of previous swap data.
 *  - Dirty page tracking & explicit flushing.
 *  - Separation of read vs write access: get_read_ptr() does not mark dirty,
 *    while get_write_ptr() (and legacy get_ptr()) marks dirty.
 *  - VMPtr<T> performs lazy allocation and swap-in, supports pointer arithmetic and indexing, and keeps write intent explicit.
 *  - Containers (vector / array / string) using pages as backing storage with iterators (including reverse iterators)
 *    and bounds-checked at().
 *
 * Limitations:
 *  - VMArray does not call constructors/destructors for non-trivial types.
 *  - VMVector cannot expose a contiguous data() buffer because elements span pages.
 *  - VMString uses a single page (no dynamic multi-page growth).
 *
 * Usage scenario:
 *  - Helps when RAM is scarce and some data can reside in a swap file when inactive.
 *
 * Safety:
 *  - VMPtr's (page, offset) constructor is protected to avoid unsafe manual pointer creation by end users.
 *  - VMManager internals are private; only friend types (VMPtr/containers) can touch low-level paging.
 *
 * Thread safety:
 *  - Not thread-safe.
 *
 * @note Generated with assistance of GitHub Copilot.
 * @note Designed for Arduino environments supporting FS abstractions.
 */

#include <FS.h>
#include <initializer_list>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <limits>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <new>

#define VM_PAGE_SIZE   4096   ///< Size (in bytes) of a single virtual memory page.
#define VM_PAGE_COUNT  16     ///< Total number of pages managed.

/**
 * @struct VMPage
 * @brief Internal descriptor for a single virtual memory page.
 */
struct VMPage {
    bool  allocated;     ///< True if the page slot is allocated.
    bool  in_ram;        ///< True if the page currently has a RAM buffer.
    bool  can_free_ram;  ///< True if RAM can be released after swapping out.
    bool  dirty;         ///< True if RAM has unsaved modifications.
    bool  zero_filled;   ///< True if page content is known zero.
    uint8_t* ram_addr;   ///< Pointer to RAM buffer (if in_ram).
    size_t swap_offset;  ///< Offset in swap file where page content is stored.
    uint64_t last_access;///< Monotonic access counter (for potential eviction heuristics).
};

// Forward declarations for friend declarations
template<typename T> class VMPtr;
template<typename T> class VMVector;
template<typename T, size_t N> class VMArray;
class VMString;

/**
 * @class VMManager
 * @brief Singleton managing a pool of fixed-size pages with swap file backing.
 *
 * @details
 * Responsibilities:
 *  - Allocating / freeing pages.
 *  - Swapping pages in/out from a filesystem file.
 *  - Tracking dirty state and providing read/write pointers.
 *
 * Thread safety: Not thread-safe.
 */
class VMManager {
public:
    /**
     * @struct AllocOptions
     * @brief Options controlling page allocation behavior.
     */
    struct AllocOptions {
        bool can_free_ram     = true;   ///< Allow freeing RAM buffer after swap out.
        bool zero_on_alloc    = true;   ///< Zero-initialize newly allocated page.
        bool reuse_swap_data  = false;  ///< Load existing swap content instead of zeroing.
    };

    /**
     * @brief Get singleton instance.
     * @return Reference to VMManager.
     */
    static VMManager& instance() {
        static VMManager inst;
        return inst;
    }

    /**
     * @brief Initialize the manager and create a fresh swap file.
     * @param filesystem Filesystem to use (e.g. SPIFFS / LittleFS).
     * @param swap_path Path to swap file.
     * @return True on success.
     *
     * @note This is part of the minimal public API that user code may call.
     */
    bool begin(fs::FS& filesystem, const char* swap_path) {
        if (started) end();
        fs = &filesystem;
        fs->remove(swap_path);

        File create_file = fs->open(swap_path, FILE_WRITE);
        if (!create_file) return false;
        uint8_t zero[VM_PAGE_SIZE] = {0};
        for (size_t i = 0; i < page_count; i++) {
            create_file.seek(i * page_size);
            create_file.write(zero, page_size);
        }
        create_file.close();

        swap_file = fs->open(swap_path, "r+");
        if (!swap_file) return false;

        for (size_t i = 0; i < page_count; i++) {
            pages[i].allocated    = false;
            pages[i].in_ram       = false;
            pages[i].can_free_ram = true;
            pages[i].dirty        = false;
            pages[i].zero_filled  = true;
            pages[i].ram_addr     = nullptr;
            pages[i].swap_offset  = i * page_size;
            pages[i].last_access  = 0;
        }
        access_tick = 0;
        started = true;
        return true;
    }

    /**
     * @brief Flush all allocated pages (force write) and keep allocations.
     *
     * @note This is part of the minimal public API that user code may call.
     */
    void flush_all() {
        for (size_t i = 0; i < page_count; ++i)
            if (pages[i].allocated)
                swap_out((int)i, true);
    }

    /**
     * @brief Shutdown manager, flushing and freeing all pages.
     *
     * @note This is part of the minimal public API that user code may call.
     */
    void end() {
        if (!started) return;
        for (size_t i = 0; i < page_count; i++) {
            if (pages[i].allocated) {
                swap_out((int)i, false);
                free_page((int)i);
            } else if (pages[i].ram_addr) {
                free(pages[i].ram_addr);
                pages[i].ram_addr = nullptr;
            }
        }
        if (swap_file) {
            swap_file.flush();
            swap_file.close();
        }
        fs = nullptr;
        started = false;
    }

    /**
     * @brief Get current page size (bytes).
     * @return Page size.
     *
     * @note Minimal public accessor; safe for user code.
     */
    size_t get_page_size() const { return page_size; }

    /**
     * @brief Get number of pages.
     * @return Page count.
     *
     * @note Minimal public accessor; safe for user code.
     */
    size_t get_page_count() const { return page_count; }

private:
    VMManager() : started(false), access_tick(0) {
        default_alloc_options.zero_on_alloc = true;
        default_alloc_options.reuse_swap_data = false;
        default_alloc_options.can_free_ram   = true;
    }
    VMManager(const VMManager&) = delete;
    VMManager& operator=(const VMManager&) = delete;

    // Grant privileged access to VM friends only.
    template<typename T> friend class ::VMPtr;
    template<typename T> friend class ::VMVector;
    template<typename T, size_t N> friend class ::VMArray;
    friend class ::VMString;

    // -------------------- Private state (hidden from end users) --------------------
    VMPage pages[VM_PAGE_COUNT]; ///< Page table.
    File swap_file;              ///< Opened swap file handle.
    fs::FS* fs = nullptr;        ///< Filesystem pointer.
    size_t page_size = VM_PAGE_SIZE; ///< Current page size (constant).
    size_t page_count = VM_PAGE_COUNT; ///< Number of pages (constant).

    bool started;                    ///< True if manager initialized.
    uint64_t access_tick;            ///< Global access counter.
    AllocOptions default_alloc_options; ///< Default allocation options.

    // -------------------- Private helpers (used by friends) --------------------

    /**
     * @brief Allocate a page with extended options (first free slot).
     * @param opts Allocation options.
     * @param out_idx Optional output page index.
     * @return Pointer to page RAM buffer or nullptr on failure.
     */
    uint8_t* alloc_page_ex(const AllocOptions& opts, int* out_idx = nullptr) {
        for (size_t i = 0; i < page_count; i++) {
            VMPage& pg = pages[i];
            if (!pg.allocated) {
                pg.ram_addr = (uint8_t*)malloc(page_size);
                if (!pg.ram_addr) return nullptr;
                pg.allocated    = true;
                pg.in_ram       = true;
                pg.can_free_ram = opts.can_free_ram;
                pg.last_access  = ++access_tick;

                if (opts.reuse_swap_data) {
                    swap_file.seek(pg.swap_offset);
                    swap_file.read(pg.ram_addr, page_size);
                    pg.dirty = false;
                    pg.zero_filled = false;
                } else {
                    if (opts.zero_on_alloc) {
                        memset(pg.ram_addr, 0, page_size);
                        pg.zero_filled = true;
                    } else {
                        pg.zero_filled = false;
                    }
                    pg.dirty = true; // initial content must be persisted
                }

                if (out_idx) *out_idx = (int)i;
                return pg.ram_addr;
            }
        }
        return nullptr;
    }

    /**
     * @brief Allocate the specific page index (no scan), if it is free.
     * @param idx Page index to allocate.
     * @param opts Allocation options.
     * @return Pointer to RAM buffer or nullptr on failure.
     *
     * @note Used by VMPtr to auto-allocate on-demand at a specific index.
     */
    uint8_t* alloc_page_at(int idx, const AllocOptions& opts) {
        if (!valid_index(idx)) return nullptr;
        VMPage& pg = pages[idx];
        if (pg.allocated) {
            // Already allocated, ensure in RAM (swap-in) and return pointer.
            if (!pg.in_ram || !pg.ram_addr) {
                if (!swap_in(idx)) return nullptr;
            }
            return pg.ram_addr;
        }
        pg.ram_addr = (uint8_t*)malloc(page_size);
        if (!pg.ram_addr) return nullptr;
        pg.allocated    = true;
        pg.in_ram       = true;
        pg.can_free_ram = opts.can_free_ram;
        pg.last_access  = ++access_tick;

        if (opts.reuse_swap_data) {
            swap_file.seek(pg.swap_offset);
            swap_file.read(pg.ram_addr, page_size);
            pg.dirty = false;
            pg.zero_filled = false;
        } else {
            if (opts.zero_on_alloc) {
                memset(pg.ram_addr, 0, page_size);
                pg.zero_filled = true;
            } else {
                pg.zero_filled = false;
            }
            pg.dirty = true;
        }
        return pg.ram_addr;
    }

    /**
     * @brief Allocate a page (simplified options).
     * @param out_idx Optional output page index.
     * @param can_free_ram Whether the page may free RAM after swap out.
     * @return Pointer to RAM buffer or nullptr.
     */
    uint8_t* alloc_page(int* out_idx = nullptr, bool can_free_ram = true) {
        AllocOptions opts = default_alloc_options;
        opts.can_free_ram = can_free_ram;
        return alloc_page_ex(opts, out_idx);
    }

    /**
     * @brief Swap a page out to disk if dirty; optionally force write.
     * @param idx Page index.
     * @param force If true, write even if not dirty.
     * @return True on success.
     */
    bool swap_out(int idx, bool force = false) {
        if (!valid_index(idx)) return false;
        VMPage& page = pages[idx];
        if (!page.allocated) return false;
        if (!page.in_ram || !page.ram_addr) return true;

        if (page.dirty || force) {
            swap_file.seek(page.swap_offset);
            size_t written = swap_file.write(page.ram_addr, page_size);
            swap_file.flush();
            (void)written;
            page.dirty = false;
        }
        if (page.can_free_ram) {
            free(page.ram_addr);
            page.ram_addr = nullptr;
            page.in_ram = false;
        }
        return true;
    }

    /**
     * @brief Ensure a page is loaded into RAM.
     * @param idx Page index.
     * @return True on success.
     */
    bool swap_in(int idx) {
        if (!valid_index(idx)) return false;
        VMPage& page = pages[idx];
        if (!page.allocated) return false;
        if (!page.in_ram || !page.ram_addr) {
            page.ram_addr = (uint8_t*)malloc(page_size);
            if (!page.ram_addr) return false;
            page.in_ram = true;
        }
        swap_file.seek(page.swap_offset);
        size_t readed = swap_file.read(page.ram_addr, page_size);
        (void)readed;
        page.last_access = ++access_tick;
        page.dirty = false;
        return true;
    }

    /**
     * @brief Prefetch a page (alias of swap_in()).
     * @param idx Page index.
     * @return True on success.
     */
    bool prefetch_page(int idx) { return swap_in(idx); }

    /**
     * @brief Legacy pointer getter (write intent). Marks page dirty.
     * @param page_idx Page index.
     * @param offset Offset inside page.
     * @return Pointer or nullptr.
     */
    void* get_ptr(int page_idx, size_t offset) {
        return get_ptr_internal(page_idx, offset, true);
    }

    /**
     * @brief Read-only pointer getter (does not mark dirty).
     * @param page_idx Page index.
     * @param offset Offset inside page.
     * @return Pointer or nullptr.
     */
    void* get_read_ptr(int page_idx, size_t offset) {
        return get_ptr_internal(page_idx, offset, false);
    }

    /**
     * @brief Write pointer getter (marks page dirty).
     * @param page_idx Page index.
     * @param offset Offset inside page.
     * @return Pointer or nullptr.
     */
    void* get_write_ptr(int page_idx, size_t offset) {
        return get_ptr_internal(page_idx, offset, true);
    }

    /**
     * @brief Mark entire page dirty.
     * @param idx Page index.
     */
    void mark_dirty(int idx) {
        if (!valid_index(idx)) return;
        VMPage& page = pages[idx];
        if (page.allocated) page.dirty = true;
    }

    /**
     * @brief Mark entire page clean.
     * @param idx Page index.
     */
    void mark_clean(int idx) {
        if (!valid_index(idx)) return;
        VMPage& page = pages[idx];
        if (page.allocated) page.dirty = false;
    }

    /**
     * @brief Mark portion of a page dirty (currently marks whole page).
     * @param idx Page index.
     * @param offset Byte offset.
     * @param len Length in bytes.
     */
    void mark_dirty_range(int idx, size_t /*offset*/, size_t /*len*/) {
        mark_dirty(idx);
    }

    /**
     * @brief Flush (force write) a page to disk.
     * @param idx Page index.
     * @return True on success.
     */
    bool flush_page(int idx) { return swap_out(idx, true); }

    /**
     * @brief Free a page and optionally wipe its swap area.
     * @param idx Page index.
     * @param wipe If true, overwrite swap content with zeros.
     * @return True on success.
     */
    bool free_page(int idx, bool wipe = false) {
        if (!valid_index(idx)) return false;
        VMPage& page = pages[idx];
        if (!page.allocated) return true;

        if (page.in_ram && page.ram_addr) {
            if (!wipe) swap_out(idx, false);
        }

        if (wipe) {
            uint8_t zero[VM_PAGE_SIZE] = {0};
            swap_file.seek(page.swap_offset);
            swap_file.write(zero, page_size);
            swap_file.flush();
        }

        if (page.ram_addr) {
            free(page.ram_addr);
            page.ram_addr = nullptr;
        }
        page.in_ram = false;
        page.allocated = false;
        page.dirty = false;
        page.zero_filled = true;
        page.last_access = ++access_tick;
        return true;
    }

    /**
     * @brief Set default allocation options for future alloc_page() calls.
     * @param opts Options.
     */
    void set_default_alloc_options(const AllocOptions& opts) {
        default_alloc_options = opts;
    }

    /**
     * @brief Get default allocation options.
     * @return Current defaults.
     */
    AllocOptions get_default_alloc_options() const {
        return default_alloc_options;
    }

    /**
     * @brief Validate page index.
     * @param idx Page index.
     * @return True if within range.
     */
    bool valid_index(int idx) const {
        return idx >= 0 && idx < (int)page_count;
    }

    /**
     * @brief Internal pointer acquisition.
     * @param page_idx Page index.
     * @param offset Offset within page.
     * @param mark_dirty_flag Whether to mark page dirty.
     * @return Pointer or nullptr.
     */
    void* get_ptr_internal(int page_idx, size_t offset, bool mark_dirty_flag) {
        if (!valid_index(page_idx)) return nullptr;
        VMPage& page = pages[page_idx];
        if (!page.allocated) return nullptr;
        if (!page.in_ram) {
            if (!swap_in(page_idx)) return nullptr;
        }
        if (offset >= page_size) return nullptr;
        page.last_access = ++access_tick;
        if (mark_dirty_flag) page.dirty = true;
        return page.ram_addr + offset;
    }
};

/**
 * @class VMPtr
 * @brief Smart pointer for objects stored in virtual memory with pointer arithmetic and indexing.
 *
 * @details
 * VMPtr represents a logical pointer to an object stored in a virtual memory page.
 * It stores the page index and offset inside the page. On dereferencing or member access,
 * it transparently ensures the corresponding memory page is swapped in (loaded in RAM)
 * and returns a pointer or reference to the object of type T.
 * 
 * Supports pointer arithmetic (operator+, operator-, ++, --, etc.), indexing (operator[]), and comparisons.
 * 
 * Additional behavior:
 *  - Pages are auto-allocated on-demand. If the pointer has no page yet (page_idx == -1) or points to a
 *    not-yet-allocated page, the VMManager will allocate that page automatically on first access.
 *
 * @tparam T Object type pointed to.
 */
template<typename T>
class VMPtr {
public:
    /**
     * @brief Default constructor (null pointer).
     */
    VMPtr() : page_idx_(-1), offset_(0) {}

    /**
     * @brief Check if pointer references a valid virtual address range (index in range and offset fits page).
     * @return True if virtual position is well-formed.
     *
     * @note Allocation state is not required for validity. Pages may be lazily allocated later.
     */
    bool valid() const {
        const auto& mgr = VMManager::instance();
        return mgr.valid_index(page_idx_)
            && offset_ + sizeof(T) <= mgr.get_page_size();
    }

    /**
     * @brief Dereference pointer, ensures the page is loaded, returns reference to object.
     * @return Reference to object.
     * @throws std::runtime_error if invalid.
     */
    T& operator*() {
        ensure_loaded();
        return *ptr();
    }
    /**
     * @brief Dereference pointer, const version.
     * @return Const reference to object.
     * @throws std::runtime_error if invalid.
     */
    const T& operator*() const {
        ensure_loaded();
        return *ptr();
    }

    /**
     * @brief Member access operator.
     * @return Pointer to object.
     * @throws std::runtime_error if invalid.
     */
    T* operator->() {
        ensure_loaded();
        return ptr();
    }
    /**
     * @brief Member access operator, const version.
     * @return Const pointer to object.
     * @throws std::runtime_error if invalid.
     */
    const T* operator->() const {
        ensure_loaded();
        return ptr();
    }

    /**
     * @brief Get raw pointer to object in RAM (after swapping in).
     * @return Pointer to object.
     */
    T* get() {
        ensure_loaded();
        return ptr();
    }
    /**
     * @brief Get const raw pointer to object in RAM (after swapping in).
     * @return Const pointer to object.
     */
    const T* get() const {
        ensure_loaded();
        return ptr();
    }

    /**
     * @brief Get page index.
     * @return Page index.
     */
    int page_index() const { return page_idx_; }

    /**
     * @brief Get offset inside the page.
     * @return Offset in bytes.
     */
    size_t page_offset() const { return offset_; }

    /**
     * @brief Equality comparison.
     * @param other Another VMPtr.
     * @return True if points to same page and offset.
     */
    bool operator==(const VMPtr& other) const {
        return page_idx_ == other.page_idx_ && offset_ == other.offset_;
    }

    /**
     * @brief Inequality comparison.
     * @param other Another VMPtr.
     * @return True if not equal.
     */
    bool operator!=(const VMPtr& other) const {
        return !(*this == other);
    }

    /**
     * @brief Less-than comparison for ordering.
     * @param other Another VMPtr.
     * @return True if this < other (by virtual address).
     */
    bool operator<(const VMPtr& other) const {
        if (page_idx_ != other.page_idx_)
            return page_idx_ < other.page_idx_;
        return offset_ < other.offset_;
    }
    bool operator>(const VMPtr& other) const { return other < *this; }
    bool operator<=(const VMPtr& other) const { return !(*this > other); }
    bool operator>=(const VMPtr& other) const { return !(*this < other); }

    /**
     * @brief Pointer arithmetic: addition (move forward n elements).
     * @param n Number of elements.
     * @return New VMPtr<T> advanced by n elements.
     */
    VMPtr operator+(ptrdiff_t n) const {
        if (!valid()) throw std::runtime_error("VMPtr: arithmetic on invalid pointer");
        const auto& mgr = VMManager::instance();
        size_t element_offset = offset_ + n * sizeof(T);
        int new_page = page_idx_;
        size_t page_size = mgr.get_page_size();
        // Move forward (or backward) across pages if needed
        if (element_offset >= page_size) {
            new_page += static_cast<int>(element_offset / page_size);
            element_offset = element_offset % page_size;
        } else if ((ptrdiff_t)element_offset < 0) {
            // Move backward across pages
            ptrdiff_t total_offset = static_cast<ptrdiff_t>(offset_) + n * static_cast<ptrdiff_t>(sizeof(T));
            while (total_offset < 0) {
                new_page -= 1;
                total_offset += page_size;
            }
            element_offset = static_cast<size_t>(total_offset);
        }
        return VMPtr(new_page, element_offset);
    }

    /**
     * @brief Pointer arithmetic: subtraction (move backward n elements).
     * @param n Number of elements.
     * @return New VMPtr<T> moved backward by n elements.
     */
    VMPtr operator-(ptrdiff_t n) const {
        return *this + (-n);
    }

    /**
     * @brief Pointer difference: number of elements between this and other.
     * @param other Another VMPtr<T> (must refer to same virtual "array").
     * @return Difference in elements.
     */
    ptrdiff_t operator-(const VMPtr& other) const {
        const auto& mgr = VMManager::instance();
        ptrdiff_t page_delta = static_cast<ptrdiff_t>(page_idx_) - static_cast<ptrdiff_t>(other.page_idx_);
        ptrdiff_t byte_delta = static_cast<ptrdiff_t>(offset_) - static_cast<ptrdiff_t>(other.offset_);
        return (page_delta * static_cast<ptrdiff_t>(mgr.get_page_size()) + byte_delta) / static_cast<ptrdiff_t>(sizeof(T));
    }

    /**
     * @brief Pre-increment: move to next element.
     * @return *this after increment.
     */
    VMPtr& operator++() { *this = *this + 1; return *this; }
    /**
     * @brief Post-increment: move to next element.
     * @return Copy before increment.
     */
    VMPtr operator++(int) { VMPtr tmp = *this; ++(*this); return tmp; }
    /**
     * @brief Pre-decrement: move to previous element.
     * @return *this after decrement.
     */
    VMPtr& operator--() { *this = *this - 1; return *this; }
    /**
     * @brief Post-decrement: move to previous element.
     * @return Copy before decrement.
     */
    VMPtr operator--(int) { VMPtr tmp = *this; --(*this); return tmp; }
    /**
     * @brief Addition assignment.
     * @param n Number of elements to move.
     * @return *this after addition.
     */
    VMPtr& operator+=(ptrdiff_t n) { *this = *this + n; return *this; }
    /**
     * @brief Subtraction assignment.
     * @param n Number of elements to move back.
     * @return *this after subtraction.
     */
    VMPtr& operator-=(ptrdiff_t n) { *this = *this - n; return *this; }

    /**
     * @brief Indexing operator: get reference to element at offset n.
     * @param n Index offset (can be negative).
     * @return Reference to element at offset.
     */
    T& operator[](ptrdiff_t n) {
        return *(*this + n);
    }
    /**
     * @brief Indexing operator: get const reference to element at offset n.
     * @param n Index offset (can be negative).
     * @return Const reference to element at offset.
     */
    const T& operator[](ptrdiff_t n) const {
        return *(*this + n);
    }

protected:
    /**
     * @brief Construct from page index and offset (protected).
     * @param page Page index in VMManager.
     * @param offset Offset in bytes inside page.
     *
     * @note Protected to prevent unsafe direct use by end users.
     *       Intended for internal operations and arithmetic within VMPtr only.
     */
    VMPtr(int page, size_t offset) : page_idx_(page), offset_(offset) {}

private:
    /**
     * @brief Ensure the referenced page is loaded into RAM, allocating it if needed.
     *
     * @throws std::runtime_error If virtual position is out of range or allocation fails.
     */
    void ensure_loaded() const {
        auto& mgr = VMManager::instance();
        // Resolve oversized offset into page/offset form if needed
        if (page_idx_ == -1) {
            // First-time use: allocate a fresh page.
            int new_idx = -1;
            if (!mgr.alloc_page(&new_idx, true))
                throw std::runtime_error("VMPtr: failed to allocate page");
            page_idx_ = new_idx;
        } else {
            if (!mgr.valid_index(page_idx_))
                throw std::runtime_error("VMPtr: page index out of range");
        }

        // If target page exists but not allocated yet, allocate it in-place.
        if (!mgr.pages[page_idx_].allocated) {
            if (!mgr.alloc_page_at(page_idx_, mgr.default_alloc_options))
                throw std::runtime_error("VMPtr: failed to allocate target page");
        }

        // Ensure offset fits within the page for object T.
        if (offset_ + sizeof(T) > mgr.get_page_size())
            throw std::runtime_error("VMPtr: object straddles page boundary");

        // Ensure page is resident.
        mgr.swap_in(page_idx_);
    }

    /**
     * @brief Get pointer to the object in RAM.
     * @return Pointer to object.
     */
    T* ptr() const {
        return reinterpret_cast<T*>(VMManager::instance().get_write_ptr(page_idx_, offset_));
    }

    mutable int page_idx_;   ///< Index of page in VMManager (auto-allocated on demand).
    size_t offset_;          ///< Offset inside the page (in bytes).
};

// -----------------------------------------------------------------------------
// detail namespace: iterator implementations
// -----------------------------------------------------------------------------
namespace detail {

/**
 * @brief Generic random-access iterator bridging container indexing.
 * @tparam Container Owning container type.
 * @tparam ValueType Element type.
 * @tparam Const True for const iterator variant.
 */
template<typename Container, typename ValueType, bool Const>
class GenericRandomAccessIterator {
public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type        = ValueType;
    using difference_type   = ptrdiff_t;
    using reference         = typename std::conditional<Const, const ValueType&, ValueType&>::type;
    using pointer           = typename std::conditional<Const, const ValueType*, ValueType*>::type;
    using ContainerPtr      = typename std::conditional<Const, const Container, Container>::type*;

    /// Default ctor (null iterator)
    GenericRandomAccessIterator() : _c(nullptr), _pos(0) {}
    /// Construct with container pointer and position.
    GenericRandomAccessIterator(ContainerPtr c, size_t pos) : _c(c), _pos(pos) {}

    reference operator*()  const { return (*_c)[_pos]; }
    pointer   operator->() const { return &(*_c)[_pos]; }

    GenericRandomAccessIterator& operator++() { ++_pos; return *this; }
    GenericRandomAccessIterator  operator++(int) { auto tmp = *this; ++_pos; return tmp; }
    GenericRandomAccessIterator& operator--() { --_pos; return *this; }
    GenericRandomAccessIterator  operator--(int) { auto tmp = *this; --_pos; return tmp; }

    GenericRandomAccessIterator& operator+=(difference_type n) { _pos += n; return *this; }
    GenericRandomAccessIterator& operator-=(difference_type n) { _pos -= n; return *this; }

    GenericRandomAccessIterator operator+(difference_type n) const { return {_c, _pos + n}; }
    GenericRandomAccessIterator operator-(difference_type n) const { return {_c, _pos - n}; }

    difference_type operator-(const GenericRandomAccessIterator& rhs) const {
        return difference_type(_pos) - difference_type(rhs._pos);
    }

    reference operator[](difference_type n) const { return (*_c)[_pos + n]; }

    bool operator==(const GenericRandomAccessIterator& other) const { return _c == other._c && _pos == other._pos; }
    bool operator!=(const GenericRandomAccessIterator& other) const { return !(*this == other); }
    bool operator<(const GenericRandomAccessIterator& other)  const { return _pos < other._pos; }
    bool operator>(const GenericRandomAccessIterator& other)  const { return _pos > other._pos; }
    bool operator<=(const GenericRandomAccessIterator& other) const { return _pos <= other._pos; }
    bool operator>=(const GenericRandomAccessIterator& other) const { return _pos >= other._pos; }

    /**
     * @brief Get current position index inside container.
     * @return Position.
     */
    size_t pos() const { return _pos; }

private:
    ContainerPtr _c; ///< Container pointer.
    size_t _pos;     ///< Logical element index.
};

/**
 * @brief Generic reverse iterator adapter over a forward random-access iterator.
 * @tparam ForwardIter Underlying forward iterator type.
 */
template<typename ForwardIter>
class GenericReverseIterator {
public:
    using iterator_type     = ForwardIter;
    using difference_type   = typename ForwardIter::difference_type;
    using value_type        = typename ForwardIter::value_type;
    using reference         = typename ForwardIter::reference;
    using pointer           = typename ForwardIter::pointer;
    using iterator_category = std::random_access_iterator_tag;

    /// Default ctor
    GenericReverseIterator() : _base() {}
    /// Construct from forward iterator (points to element after the one reverse deref will yield).
    explicit GenericReverseIterator(ForwardIter it) : _base(it) {}

    /**
     * @brief Get underlying base iterator.
     * @return Forward iterator.
     */
    ForwardIter base() const { return _base; }

    reference operator*() const {
        ForwardIter tmp = _base;
        --tmp;
        return *tmp;
    }
    pointer operator->() const { return &(**this); }

    GenericReverseIterator& operator++() { --_base; return *this; }
    GenericReverseIterator  operator++(int) { auto tmp = *this; --_base; return tmp; }
    GenericReverseIterator& operator--() { ++_base; return *this; }
    GenericReverseIterator  operator--(int) { auto tmp = *this; ++_base; return tmp; }

    GenericReverseIterator operator+(difference_type n) const { return GenericReverseIterator(_base - n); }
    GenericReverseIterator operator-(difference_type n) const { return GenericReverseIterator(_base + n); }

    GenericReverseIterator& operator+=(difference_type n) { _base -= n; return *this; }
    GenericReverseIterator& operator-=(difference_type n) { _base += n; return *this; }

    reference operator[](difference_type n) const { return *(*this + n); }

    difference_type operator-(const GenericReverseIterator& other) const {
        return other._base - _base;
    }

    bool operator==(const GenericReverseIterator& other) const { return _base == other._base; }
    bool operator!=(const GenericReverseIterator& other) const { return _base != other._base; }
    bool operator<(const GenericReverseIterator& other)  const { return other._base < _base; }
    bool operator>(const GenericReverseIterator& other)  const { return other._base > _base; }
    bool operator<=(const GenericReverseIterator& other) const { return !(other._base < _base); }
    bool operator>=(const GenericReverseIterator& other) const { return !(other._base > _base); }

private:
    ForwardIter _base; ///< Forward iterator one-past current reverse element.
};

} // namespace detail

// -----------------------------------------------------------------------------
// VMVector
// -----------------------------------------------------------------------------

/**
 * @brief Vector-like container backed by pageable storage.
 * @tparam T Element type.
 * @note Not fully STL-compatible; no allocator support. Iterators become invalid
 *       after modifications similar to std::vector in some operations.
 * @warning Elements spanning pages means no contiguous single block; data() unsupported.
 */
template<typename T>
class VMVector {
public:
    typedef T value_type;              ///< Element type alias.
    typedef T& reference;              ///< Reference type.
    typedef const T& const_reference;  ///< Const reference type.
    typedef size_t size_type;          ///< Size type.
    typedef ptrdiff_t difference_type; ///< Difference type.

    using iterator               = detail::GenericRandomAccessIterator<VMVector, T, false>;
    using const_iterator         = detail::GenericRandomAccessIterator<VMVector, T, true>;
    using reverse_iterator       = detail::GenericReverseIterator<iterator>;
    using const_reverse_iterator = detail::GenericReverseIterator<const_iterator>;

    /// Default constructor.
    VMVector() : _chunk_capacity(VM_PAGE_SIZE / sizeof(T)), _size(0), _chunk_count(0) {
        for (size_type i = 0; i < VM_PAGE_COUNT; ++i) {
            _chunks[i].page_idx = -1;
            _chunks[i].count = 0;
        }
    }
    /// Fill constructor.
    VMVector(size_type n, const T& val = T()) : VMVector() { assign(n, val); }
    /// Initializer list constructor.
    VMVector(const std::initializer_list<T>& ilist) : VMVector() { assign(ilist.begin(), ilist.end()); }
    /// Copy constructor.
    VMVector(const VMVector& other) : VMVector() { assign(other.begin(), other.end()); }

    /// Move constructor.
    VMVector(VMVector&& other) noexcept
        : _chunk_capacity(other._chunk_capacity), _size(other._size), _chunk_count(other._chunk_count) {
        for (size_type i = 0; i < VM_PAGE_COUNT; ++i) {
            _chunks[i] = other._chunks[i];
            other._chunks[i].page_idx = -1;
            other._chunks[i].count = 0;
        }
        other._size = 0;
        other._chunk_count = 0;
    }

    /// Copy assignment.
    VMVector& operator=(const VMVector& other) {
        if (this != &other) {
            clear();
            assign(other.begin(), other.end());
        }
        return *this;
    }
    /// Move assignment.
    VMVector& operator=(VMVector&& other) noexcept {
        if (this != &other) {
            clear();
            _chunk_capacity = other._chunk_capacity;
            _size           = other._size;
            _chunk_count    = other._chunk_count;
            for (size_type i = 0; i < VM_PAGE_COUNT; ++i) {
                _chunks[i] = other._chunks[i];
                other._chunks[i].page_idx = -1;
                other._chunks[i].count = 0;
            }
            other._size = 0;
            other._chunk_count = 0;
        }
        return *this;
    }

    /// Destructor.
    ~VMVector() { clear(); }

    // Element access (non-const -> write intent)
    /**
     * @brief Unchecked element access (write intent).
     * @param idx Element index.
     * @return Reference.
     */
    reference operator[](size_type idx) {
        size_type chunk_num = idx / _chunk_capacity;
        size_type offset    = idx % _chunk_capacity;
        Chunk& ch = _chunks[chunk_num];
        return *reinterpret_cast<T*>(VMManager::instance().get_write_ptr(ch.page_idx, offset * sizeof(T)));
    }
    /**
     * @brief Unchecked element access (read intent).
     * @param idx Element index.
     * @return Const reference.
     */
    const_reference operator[](size_type idx) const {
        size_type chunk_num = idx / _chunk_capacity;
        size_type offset    = idx % _chunk_capacity;
        const Chunk& ch = _chunks[chunk_num];
        return *reinterpret_cast<const T*>(VMManager::instance().get_read_ptr(ch.page_idx, offset * sizeof(T)));
    }
    /**
     * @brief Bounds-checked element access.
     * @param idx Index.
     * @throws std::out_of_range If idx >= size().
     */
    reference at(size_type idx) {
        if (idx >= _size) throw std::out_of_range("VMVector::at");
        return (*this)[idx];
    }
    /**
     * @brief Bounds-checked element access (const).
     * @param idx Index.
     * @throws std::out_of_range If idx >= size().
     */
    const_reference at(size_type idx) const {
        if (idx >= _size) throw std::out_of_range("VMVector::at const");
        return (*this)[idx];
    }
    /// Get first element (throws if empty).
    reference front() {
        if (_size == 0) throw std::out_of_range("VMVector::front");
        return (*this)[0];
    }
    /// Get first element (const).
    const_reference front() const {
        if (_size == 0) throw std::out_of_range("VMVector::front const");
        return (*this)[0];
    }
    /// Get last element (throws if empty).
    reference back() {
        if (_size == 0) throw std::out_of_range("VMVector::back");
        return (*this)[_size - 1];
    }
    /// Get last element (const).
    const_reference back() const {
        if (_size == 0) throw std::out_of_range("VMVector::back const");
        return (*this)[_size - 1];
    }

    /// Check if empty.
    bool empty() const { return _size == 0; }
    /// Number of elements.
    size_type size() const { return _size; }
    /// Current capacity in elements (sum of allocated chunks).
    size_type capacity() const { return _chunk_count * _chunk_capacity; }

    /**
     * @brief Append element by copy.
     * @param value Value to copy.
     */
    void push_back(const T& value) {
        ensure_back_slot();
        Chunk& ch = _chunks[_chunk_count - 1];
        T* ptr = reinterpret_cast<T*>(VMManager::instance().get_write_ptr(ch.page_idx, ch.count * sizeof(T)));
        new(ptr) T(value);
        ch.count++; _size++;
    }

    /**
     * @brief In-place construct element at end.
     * @tparam Args Constructor argument types.
     * @param args Arguments.
     * @return Reference to new element.
     */
    template<typename... Args>
    reference emplace_back(Args&&... args) {
        ensure_back_slot();
        Chunk& ch = _chunks[_chunk_count - 1];
        T* ptr = reinterpret_cast<T*>(VMManager::instance().get_write_ptr(ch.page_idx, ch.count * sizeof(T)));
        new(ptr) T(std::forward<Args>(args)...);
        ch.count++; _size++;
        return *ptr;
    }

    /**
     * @brief Emplace element at arbitrary position (shifts subsequent).
     * @param pos Iterator position.
     * @tparam Args Constructor argument types.
     * @param args Constructor arguments.
     * @return Iterator to inserted element.
     */
    template<typename... Args>
    iterator emplace(iterator pos, Args&&... args) {
        size_type idx = pos - begin();
        emplace_back(); // creates space at end
        for (size_type i = _size - 1; i > idx; --i) {
            (*this)[i] = std::move((*this)[i - 1]);
        }
        (*this)[idx] = T(std::forward<Args>(args)...);
        return iterator(this, idx);
    }

    /**
     * @brief Remove last element.
     * @throws std::out_of_range If empty.
     */
    void pop_back() {
        if (_size == 0) throw std::out_of_range("VMVector::pop_back");
        _size--;
        size_type chunk_num = _size / _chunk_capacity;
        size_type offset    = _size % _chunk_capacity;
        if (offset == 0 && chunk_num > 0) chunk_num--;
        Chunk& ch = _chunks[chunk_num];
        T* ptr = reinterpret_cast<T*>(VMManager::instance().get_write_ptr(ch.page_idx, (ch.count - 1) * sizeof(T)));
        ptr->~T();
        ch.count--;
        if (ch.count == 0) {
            VMManager::instance().free_page(ch.page_idx);
            ch.page_idx = -1;
            _chunk_count--;
        }
    }

    /**
     * @brief Insert element by copy.
     * @param pos Target insertion position.
     * @param value Value to insert.
     * @return Iterator to new element.
     */
    iterator insert(iterator pos, const T& value) {
        size_type idx = pos - begin();
        push_back(T());
        for (size_type i = _size - 1; i > idx; --i)
            (*this)[i] = (*this)[i - 1];
        (*this)[idx] = value;
        return iterator(this, idx);
    }

    /**
     * @brief Erase element at position.
     * @param pos Iterator to element.
     * @return Iterator to position that held erased element.
     */
    iterator erase(iterator pos) {
        size_type idx = pos - begin();
        if (idx >= _size) return end();
        for (size_type i = idx; i < _size - 1; ++i)
            (*this)[i] = (*this)[i + 1];
        pop_back();
        return iterator(this, idx);
    }

    /**
     * @brief Destroy all elements and free pages.
     */
    void clear() {
        for (size_type i = 0; i < _chunk_count; ++i) {
            Chunk& ch = _chunks[i];
            if (ch.page_idx == -1) continue;
            for (size_type j = 0; j < ch.count; ++j) {
                T* ptr = reinterpret_cast<T*>(VMManager::instance().get_write_ptr(ch.page_idx, j * sizeof(T)));
                ptr->~T();
            }
            VMManager::instance().free_page(ch.page_idx);
            ch.page_idx = -1;
            ch.count = 0;
        }
        _chunk_count = 0;
        _size = 0;
    }

    /**
     * @brief Resize container.
     * @param n New size.
     * @param val Fill value for new elements.
     */
    void resize(size_type n, const T& val = T()) {
        if (n < _size) {
            while (_size > n) pop_back();
        } else if (n > _size) {
            while (_size < n) push_back(val);
        }
    }

    /**
     * @brief Reserve capacity for at least n elements.
     * @param n Desired capacity.
     */
    void reserve(size_type n) {
        size_type required_chunks = (n + _chunk_capacity - 1) / _chunk_capacity;
        while (_chunk_count < required_chunks) {
            int page_idx;
            VMManager::instance().alloc_page(&page_idx, true);
            _chunks[_chunk_count].page_idx = page_idx;
            _chunks[_chunk_count].count = 0;
            _chunk_count++;
        }
    }

    /**
     * @brief Release unused trailing pages.
     */
    void shrink_to_fit() {
        size_type used_chunks = (_size + _chunk_capacity - 1) / _chunk_capacity;
        for (size_type i = used_chunks; i < _chunk_count; ++i) {
            if (_chunks[i].page_idx != -1) {
                VMManager::instance().free_page(_chunks[i].page_idx);
                _chunks[i].page_idx = -1;
                _chunks[i].count = 0;
            }
        }
        _chunk_count = used_chunks;
    }

    /**
     * @brief Swap contents with another VMVector.
     * @param other Other vector.
     */
    void swap(VMVector& other) {
        std::swap(_chunk_capacity, other._chunk_capacity);
        std::swap(_size, other._size);
        std::swap(_chunk_count, other._chunk_count);
        for (size_type i = 0; i < VM_PAGE_COUNT; ++i)
            std::swap(_chunks[i], other._chunks[i]);
    }

    /**
     * @brief Assign n copies of value.
     * @param n Count.
     * @param val Value.
     */
    void assign(size_type n, const T& val) {
        clear();
        resize(n, val);
    }
    /**
     * @brief Assign from iterator range.
     * @tparam InputIt Iterator type.
     * @param first Begin.
     * @param last End.
     */
    template<typename InputIt>
    void assign(InputIt first, InputIt last) {
        clear();
        for (InputIt it = first; it != last; ++it) push_back(*it);
    }

    // Iterators
    iterator begin() { return iterator(this, 0); }
    iterator end()   { return iterator(this, _size); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end()   const { return const_iterator(this, _size); }
    const_iterator cbegin() const { return const_iterator(this, 0); }
    const_iterator cend()   const { return const_iterator(this, _size); }

    reverse_iterator rbegin() { return reverse_iterator(end()); }
    reverse_iterator rend()   { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend()   const { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator crend()   const { return const_reverse_iterator(begin()); }

    // Comparisons
    /**
     * @brief Equality comparison.
     * @param other Other vector.
     * @return True if sizes and elements equal.
     */
    bool operator==(const VMVector& other) const {
        if (_size != other._size) return false;
        for (size_type i = 0; i < _size; ++i)
            if ((*this)[i] != other[i]) return false;
        return true;
    }
    bool operator!=(const VMVector& other) const { return !(*this == other); }
    bool operator<(const VMVector& other) const {
        size_type n = std::min(_size, other._size);
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
    /**
     * @brief Internal chunk descriptor (one page).
     */
    struct Chunk {
        int page_idx;   ///< Page index in VMManager.
        size_type count;///< Number of constructed elements in this page.
    };

    Chunk _chunks[VM_PAGE_COUNT]; ///< Fixed chunk table (one per possible page).
    size_type _chunk_count;       ///< Active chunk count.
    size_type _chunk_capacity;    ///< Elements per chunk.
    size_type _size;              ///< Total elements.

    /**
     * @brief Ensure space for one more element, allocate new page if needed.
     */
    void ensure_back_slot() {
        if (_chunk_count == 0 || _chunks[_chunk_count - 1].count >= _chunk_capacity) {
            int page_idx;
            VMManager::instance().alloc_page(&page_idx, true);
            _chunks[_chunk_count].page_idx = page_idx;
            _chunks[_chunk_count].count = 0;
            _chunk_count++;
        }
    }
};

// -----------------------------------------------------------------------------
// VMArray
// -----------------------------------------------------------------------------

/**
 * @brief Fixed-size array backed by a single page.
 * @tparam T Element type.
 * @tparam N Number of elements.
 * @warning Does not invoke constructors/destructors for non-trivial types.
 */
template<typename T, size_t N>
class VMArray {
public:
    typedef T value_type;
    typedef T& reference;
    typedef const T& const_reference;
    typedef size_t size_type;
    typedef ptrdiff_t difference_type;

    using iterator               = detail::GenericRandomAccessIterator<VMArray, T, false>;
    using const_iterator         = detail::GenericRandomAccessIterator<VMArray, T, true>;
    using reverse_iterator       = detail::GenericReverseIterator<iterator>;
    using const_reverse_iterator = detail::GenericReverseIterator<const_iterator>;

    /// Constructor allocates one page.
    VMArray() { VMManager::instance().alloc_page(&page_idx, true); }
    /// Destructor frees page.
    ~VMArray() {
        if (page_idx >= 0) {
            VMManager::instance().free_page(page_idx);
            page_idx = -1;
        }
    }

    /**
     * @brief Unchecked element access (write intent).
     * @param idx Index.
     * @return Reference.
     */
    reference operator[](size_type idx) {
        return *reinterpret_cast<T*>(VMManager::instance().get_write_ptr(page_idx, idx * sizeof(T)));
    }
    /**
     * @brief Unchecked element access (read intent).
     * @param idx Index.
     * @return Const reference.
     */
    const_reference operator[](size_type idx) const {
        return *reinterpret_cast<const T*>(VMManager::instance().get_read_ptr(page_idx, idx * sizeof(T)));
    }
    /**
     * @brief Bounds-checked access.
     * @param idx Index.
     * @throws std::out_of_range If idx >= N.
     */
    reference at(size_type idx) {
        if (idx >= N) throw std::out_of_range("VMArray::at");
        return (*this)[idx];
    }
    /**
     * @brief Bounds-checked access (const).
     * @param idx Index.
     * @throws std::out_of_range If idx >= N.
     */
    const_reference at(size_type idx) const {
        if (idx >= N) throw std::out_of_range("VMArray::at const");
        return (*this)[idx];
    }

    /// Number of elements.
    size_type size()  const { return N; }
    /// True if empty (N == 0).
    bool empty()      const { return N == 0; }

    /**
     * @brief Fill all elements with value.
     * @param val Value.
     */
    void fill(const T& val) {
        for (size_type i = 0; i < N; ++i) (*this)[i] = val;
    }

    /**
     * @brief Reset array elements to default constructed T() and swap out page.
     */
    void clear() {
        for (size_type i = 0; i < N; ++i)
            (*this)[i] = T();
        VMManager::instance().swap_out(page_idx);
    }

    // Iterators
    iterator begin() { return iterator(this, 0); }
    iterator end()   { return iterator(this, N); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end()   const { return const_iterator(this, N); }
    const_iterator cbegin() const { return const_iterator(this, 0); }
    const_iterator cend()   const { return const_iterator(this, N); }

    reverse_iterator rbegin() { return reverse_iterator(end()); }
    reverse_iterator rend()   { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend()   const { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator crend()   const { return const_reverse_iterator(begin()); }

private:
    int page_idx; ///< Page index backing the array.
};

// -----------------------------------------------------------------------------
// VMString
// -----------------------------------------------------------------------------

/**
 * @brief Single-page mutable string with limited capacity (page_size - 1).
 *
 * @details No multi-page growth; exceeding capacity throws. Provides many
 * std::string-like operations (append, insert, replace, find, etc.).
 */
class VMString {
public:
    typedef char value_type;
    typedef char& reference;
    typedef const char& const_reference;
    typedef size_t size_type;
    typedef ptrdiff_t difference_type;

    using iterator               = detail::GenericRandomAccessIterator<VMString, char, false>;
    using const_iterator         = detail::GenericRandomAccessIterator<VMString, char, true>;
    using reverse_iterator       = detail::GenericReverseIterator<iterator>;
    using const_reverse_iterator = detail::GenericReverseIterator<const_iterator>;

    static constexpr size_type npos = static_cast<size_type>(-1); ///< Not-found value.

    /**
     * @brief Construct with initial capacity (rounded to page capacity).
     * @param initial_capacity Hint (still limited to one page).
     */
    explicit VMString(size_t initial_capacity = 64)
        : _page_idx(-1), _buf(nullptr), _size(0), _capacity(0) {
        allocate_initial_page(initial_capacity);
        // Always refresh pointer through write path to ensure residency.
        char* b = write_buf();
        b[0] = '\0';
    }

    /// Construct from C-string.
    VMString(const char* s) : VMString(strlen(s) + 1) { assign(s); }
    /// Construct from char pointer with explicit count.
    VMString(const char* s, size_type count) : VMString(count + 1) { assign(s, count); }
    /// Construct fill string (count copies of ch).
    VMString(size_type count, char ch) : VMString(count + 1) { assign(count, ch); }
    /// Copy constructor.
    VMString(const VMString& other) : VMString(other._size + 1) { assign(other.c_str(), other._size); }

    /// Move constructor.
    VMString(VMString&& other) noexcept
        : _page_idx(other._page_idx), _buf(other._buf),
          _size(other._size), _capacity(other._capacity) {
        other._page_idx = -1;
        other._buf = nullptr;
        other._size = 0;
        other._capacity = 0;
    }

    /// Copy assignment.
    VMString& operator=(const VMString& other) {
        if (this != &other) assign(other.c_str(), other._size);
        return *this;
    }
    /// Move assignment.
    VMString& operator=(VMString&& other) noexcept {
        if (this != &other) {
            if (_page_idx >= 0) VMManager::instance().free_page(_page_idx);
            _page_idx = other._page_idx;
            _buf      = other._buf;
            _size     = other._size;
            _capacity = other._capacity;
            other._page_idx = -1;
            other._buf = nullptr;
            other._size = 0;
            other._capacity = 0;
        }
        return *this;
    }

    /// Destructor frees page.
    ~VMString() {
        if (_page_idx >= 0) {
            VMManager::instance().free_page(_page_idx);
            _buf = nullptr;
            _page_idx = -1;
        }
    }

    // Iterators
    iterator begin() { return iterator(this, 0); }
    iterator end()   { return iterator(this, _size); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end()   const { return const_iterator(this, _size); }
    const_iterator cbegin() const { return const_iterator(this, 0); }
    const_iterator cend()   const { return const_iterator(this, _size); }

    reverse_iterator rbegin() { return reverse_iterator(end()); }
    reverse_iterator rend()   { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend()   const { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator crend()   const { return const_reverse_iterator(begin()); }

    // Element access
    /**
     * @brief Bounds-checked mutable character access.
     * @param pos Index.
     * @throws std::out_of_range If pos >= size().
     */
    reference at(size_type pos) {
        if (pos >= _size) throw std::out_of_range("VMString::at");
        return write_buf()[pos];
    }
    /**
     * @brief Bounds-checked const access.
     * @param pos Index.
     * @throws std::out_of_range If pos >= size().
     */
    const_reference at(size_type pos) const {
        if (pos >= _size) throw std::out_of_range("VMString::at const");
        return read_buf()[pos];
    }
    /**
     * @brief Unchecked mutable access.
     * @param idx Index.
     */
    reference operator[](size_type idx) {
        if (idx >= _size) throw std::out_of_range("VMString::operator[]");
        return write_buf()[idx];
    }
    /**
     * @brief Unchecked const access.
     * @param idx Index.
     */
    const_reference operator[](size_type idx) const {
        if (idx >= _size) throw std::out_of_range("VMString::operator[] const");
        return read_buf()[idx];
    }
    /// Get first character.
    reference front() {
        if (empty()) throw std::out_of_range("VMString::front");
        return write_buf()[0];
    }
    /// Get first character (const).
    const_reference front() const {
        if (empty()) throw std::out_of_range("VMString::front const");
        return read_buf()[0];
    }
    /// Get last character.
    reference back() {
        if (empty()) throw std::out_of_range("VMString::back");
        return write_buf()[_size - 1];
    }
    /// Get last character (const).
    const_reference back() const {
        if (empty()) throw std::out_of_range("VMString::back const");
        return read_buf()[_size - 1];
    }

    /**
     * @brief Get C-string pointer (null-terminated).
     * @return Pointer (never null).
     */
    const char* c_str() const {
        // Always refresh pointer via read access. If page not present (moved-from), return empty literal.
        return (_page_idx >= 0) ? read_buf() : "";
    }

    // Capacity
    bool empty() const { return _size == 0; }
    size_type size() const { return _size; }
    size_type length() const { return _size; }
    size_type capacity() const { return _capacity; }
    /**
     * @brief Maximum storable characters (excluding null terminator).
     * @return Limit (page_size - 1).
     */
    size_type max_size() const { return VMManager::instance().get_page_size() - 1; }

    /**
     * @brief Ensure capacity >= new_cap.
     * @param new_cap Desired capacity.
     * @throws std::length_error If exceeds single page.
     */
    void reserve(size_type new_cap) {
        if (new_cap <= _capacity) return;
        if (new_cap > max_size()) throw std::length_error("VMString::reserve exceeds max_size()");
        reallocate_page(new_cap);
    }
    /// No-op (single page).
    void shrink_to_fit() { /* no-op single page */ }

    /**
     * @brief Resize string (fill with ch if expanding).
     * @param new_size New size.
     * @param ch Fill character.
     */
    void resize(size_type new_size, char ch = '\0') {
        if (new_size > max_size()) throw std::length_error("VMString::resize exceeds one page");
        ensure_capacity(new_size + 1);
        char* buf = write_buf();
        if (new_size > _size)
            memset(buf + _size, ch, new_size - _size);
        _size = new_size;
        buf[_size] = '\0';
    }

    // Assign
    /**
     * @brief Assign from C-string.
     * @param s Source string.
     */
    void assign(const char* s) { assign(s, strlen(s)); }
    /**
     * @brief Assign from buffer.
     * @param s Source pointer.
     * @param count Characters count.
     */
    void assign(const char* s, size_type count) {
        ensure_capacity(count + 1);
        char* buf = write_buf();
        if (count) memcpy(buf, s, count);
        _size = count;
        buf[_size] = '\0';
    }
    /**
     * @brief Assign repeated character.
     * @param count Repetitions.
     * @param ch Character.
     */
    void assign(size_type count, char ch) {
        ensure_capacity(count + 1);
        char* buf = write_buf();
        memset(buf, ch, count);
        _size = count;
        buf[_size] = '\0';
    }
    /**
     * @brief Assign substring of another VMString.
     * @param other Source string.
     * @param pos Starting position.
     * @param count Max characters.
     */
    void assign(const VMString& other, size_type pos, size_type count = npos) {
        if (pos > other._size) throw std::out_of_range("VMString::assign(pos)");
        size_type rcount = std::min(count, other._size - pos);
        assign(other.c_str() + pos, rcount);
    }

    // Append
    VMString& append(const char* s) { return append(s, strlen(s)); }
    VMString& append(const char* s, size_type count) {
        ensure_capacity(_size + count + 1);
        char* buf = write_buf();
        if (count) memcpy(buf + _size, s, count);
        _size += count;
        buf[_size] = '\0';
        return *this;
    }
    VMString& append(const VMString& other) { return append(other.c_str(), other._size); }
    VMString& append(size_type count, char ch) {
        ensure_capacity(_size + count + 1);
        char* buf = write_buf();
        memset(buf + _size, ch, count);
        _size += count;
        buf[_size] = '\0';
        return *this;
    }

    /**
     * @brief Push one character at end.
     * @param c Character.
     */
    void push_back(char c) {
        ensure_capacity(_size + 2);
        char* buf = write_buf();
        buf[_size++] = c;
        buf[_size] = '\0';
    }
    /**
     * @brief Remove last character.
     */
    void pop_back() {
        if (empty()) throw std::out_of_range("VMString::pop_back");
        char* buf = write_buf();
        _size--;
        buf[_size] = '\0';
    }

    VMString& operator+=(const char* s) { return append(s); }
    VMString& operator+=(const VMString& s) { return append(s); }
    VMString& operator+=(char c) { push_back(c); return *this; }

    // Insert
    VMString& insert(size_type pos, const char* s) { return insert(pos, s, strlen(s)); }
    VMString& insert(size_type pos, const char* s, size_type count) {
        if (pos > _size) throw std::out_of_range("VMString::insert");
        ensure_capacity(_size + count + 1);
        char* buf = write_buf();
        memmove(buf + pos + count, buf + pos, _size - pos);
        if (count) memcpy(buf + pos, s, count);
        _size += count;
        buf[_size] = '\0';
        return *this;
    }
    VMString& insert(size_type pos, size_type count, char ch) {
        if (pos > _size) throw std::out_of_range("VMString::insert");
        ensure_capacity(_size + count + 1);
        char* buf = write_buf();
        memmove(buf + pos + count, buf + pos, _size - pos);
        memset(buf + pos, ch, count);
        _size += count;
        buf[_size] = '\0';
        return *this;
    }
    VMString& insert(size_type pos, const VMString& other) {
        return insert(pos, other.c_str(), other._size);
    }

    // Erase
    /**
     * @brief Erase substring.
     * @param pos Start position.
     * @param count Characters to erase (npos -> to end).
     * @return *this
     */
    VMString& erase(size_type pos = 0, size_type count = npos) {
        if (pos > _size) throw std::out_of_range("VMString::erase");
        size_type rcount = std::min(count, _size - pos);
        char* buf = write_buf();
        memmove(buf + pos, buf + pos + rcount, _size - pos - rcount);
        _size -= rcount;
        buf[_size] = '\0';
        return *this;
    }

    // Replace
    VMString& replace(size_type pos, size_type count, const char* s, size_type s_count) {
        if (pos > _size) throw std::out_of_range("VMString::replace");
        size_type rcount = std::min(count, _size - pos);
        char* buf = write_buf();
        if (s_count != rcount) {
            if (s_count > rcount) ensure_capacity(_size + (s_count - rcount) + 1);
            // Refresh write buffer again in case capacity changed (page may be reallocated)
            buf = write_buf();
            memmove(buf + pos + s_count, buf + pos + rcount, _size - (pos + rcount));
            _size = _size - rcount + s_count;
        }
        if (s_count) memcpy(buf + pos, s, s_count);
        buf[_size] = '\0';
        return *this;
    }
    VMString& replace(size_type pos, size_type count, const char* s) {
        return replace(pos, count, s, strlen(s));
    }
    VMString& replace(size_type pos, size_type count, const VMString& other) {
        return replace(pos, count, other.c_str(), other._size);
    }

    // Substring
    /**
     * @brief Create substring.
     * @param pos Starting offset.
     * @param count Length (npos => to end).
     * @return New VMString.
     */
    VMString substr(size_type pos = 0, size_type count = npos) const {
        if (pos > _size) throw std::out_of_range("VMString::substr");
        size_type rcount = std::min(count, _size - pos);
        return VMString(c_str() + pos, rcount);
    }

    // Copy
    /**
     * @brief Copy characters to external buffer.
     * @param dest Destination buffer.
     * @param count Max chars.
     * @param pos Source offset.
     * @return Number of characters copied.
     */
    size_type copy(char* dest, size_type count, size_type pos = 0) const {
        if (pos > _size) throw std::out_of_range("VMString::copy");
        size_type rcount = std::min(count, _size - pos);
        const char* buf = read_buf();
        if (rcount) memcpy(dest, buf + pos, rcount);
        return rcount;
    }

    // Swap
    /**
     * @brief Swap contents.
     * @param other Other string.
     */
    void swap(VMString& other) {
        std::swap(_page_idx, other._page_idx);
        std::swap(_buf, other._buf);
        std::swap(_size, other._size);
        std::swap(_capacity, other._capacity);
    }

    // Find family
    size_type find(const char* s, size_type pos, size_type count) const {
        if (count == 0) return pos <= _size ? pos : npos;
        if (count > _size) return npos;
        const char* buf = read_buf();
        for (size_type i = pos; i + count <= _size; ++i)
            if (memcmp(buf + i, s, count) == 0) return i;
        return npos;
    }
    size_type find(const char* s, size_type pos = 0) const { return find(s, pos, strlen(s)); }
    size_type find(const VMString& other, size_type pos = 0) const { return find(other.c_str(), pos, other._size); }
    size_type find(char ch, size_type pos = 0) const {
        const char* buf = read_buf();
        for (size_type i = pos; i < _size; ++i)
            if (buf[i] == ch) return i;
        return npos;
    }

    // Reverse find
    size_type rfind(const char* s, size_type pos, size_type count) const {
        if (count == 0) return std::min(pos, _size);
        if (count > _size) return npos;
        const char* buf = read_buf();
        size_type start = std::min(pos, _size - count);
        for (size_type i = start + 1; i-- > 0;) {
            if (memcmp(buf + i, s, count) == 0) return i;
            if (i == 0) break;
        }
        return npos;
    }
    size_type rfind(const char* s, size_type pos = npos) const {
        return rfind(s, pos == npos ? _size : pos, strlen(s));
    }
    size_type rfind(const VMString& other, size_type pos = npos) const {
        return rfind(other.c_str(), pos == npos ? _size : pos, other._size);
    }
    size_type rfind(char ch, size_type pos = npos) const {
        const char* buf = read_buf();
        size_type i = std::min(pos == npos ? _size : pos, _size);
        while (i-- > 0)
            if (buf[i] == ch) return i;
        return npos;
    }

    // Character class finds
    size_type find_first_of(const char* s, size_type pos = 0) const {
        const char* buf = read_buf();
        for (size_type i = pos; i < _size; ++i)
            if (strchr(s, buf[i])) return i;
        return npos;
    }
    size_type find_first_of(const VMString& other, size_type pos = 0) const {
        return find_first_of(other.c_str(), pos);
    }
    size_type find_first_of(char ch, size_type pos = 0) const {
        return find(ch, pos);
    }

    size_type find_last_of(const char* s, size_type pos = npos) const {
        const char* buf = read_buf();
        size_type i = std::min(pos == npos ? _size : pos, _size);
        while (i-- > 0)
            if (strchr(s, buf[i])) return i;
        return npos;
    }
    size_type find_last_of(const VMString& other, size_type pos = npos) const {
        return find_last_of(other.c_str(), pos);
    }
    size_type find_last_of(char ch, size_type pos = npos) const {
        return rfind(ch, pos);
    }

    size_type find_first_not_of(const char* s, size_type pos = 0) const {
        const char* buf = read_buf();
        for (size_type i = pos; i < _size; ++i)
            if (!strchr(s, buf[i])) return i;
        return npos;
    }
    size_type find_first_not_of(const VMString& other, size_type pos = 0) const {
        return find_first_not_of(other.c_str(), pos);
    }
    size_type find_first_not_of(char ch, size_type pos = 0) const {
        const char* buf = read_buf();
        for (size_type i = pos; i < _size; ++i)
            if (buf[i] != ch) return i;
        return npos;
    }

    size_type find_last_not_of(const char* s, size_type pos = npos) const {
        const char* buf = read_buf();
        size_type i = std::min(pos == npos ? _size : pos, _size);
        while (i-- > 0)
            if (!strchr(s, buf[i])) return i;
        return npos;
    }
    size_type find_last_not_of(const VMString& other, size_type pos = npos) const {
        return find_last_not_of(other.c_str(), pos);
    }
    size_type find_last_not_of(char ch, size_type pos = npos) const {
        const char* buf = read_buf();
        size_type i = std::min(pos == npos ? _size : pos, _size);
        while (i-- > 0)
            if (buf[i] != ch) return i;
        return npos;
    }

    // Compare
    /**
     * @brief Compare with another VMString.
     * @param other Other string.
     * @return Negative, 0, or positive like strcmp.
     */
    int compare(const VMString& other) const {
        const char* a = read_buf();
        const char* b = other.c_str();
        int r = std::memcmp(a, b, std::min(_size, other._size));
        if (r != 0) return r;
        if (_size == other._size) return 0;
        return _size < other._size ? -1 : 1;
    }
    /**
     * @brief Compare with C-string.
     * @param s C-string.
     * @return Comparison result.
     */
    int compare(const char* s) const {
        const char* a = read_buf();
        size_type slen = std::strlen(s);
        int r = std::memcmp(a, s, std::min(_size, slen));
        if (r != 0) return r;
        if (_size == slen) return 0;
        return _size < slen ? -1 : 1;
    }

    bool operator==(const VMString& other) const { return _size == other._size && std::memcmp(read_buf(), other.c_str(), _size) == 0; }
    bool operator!=(const VMString& other) const { return !(*this == other); }
    bool operator<(const VMString& other)  const { return compare(other) < 0; }
    bool operator>(const VMString& other)  const { return compare(other) > 0; }
    bool operator<=(const VMString& other) const { return compare(other) <= 0; }
    bool operator>=(const VMString& other) const { return compare(other) >= 0; }

    /**
     * @brief Clear content (size = 0) and swap page out, leaving no dangling _buf.
     *
     * @details After swap_out(), if the page is allowed to free RAM, any cached pointer becomes invalid.
     *          We explicitly set _buf to nullptr to avoid a dangling pointer.
     */
    void clear() {
        if (_page_idx >= 0) {
            char* buf = write_buf();
            buf[0] = '\0';
            _size = 0;
            VMManager::instance().swap_out(_page_idx);
            _buf = nullptr; // avoid stale pointer after potential RAM free
        } else {
            _size = 0;
        }
    }

private:
    int _page_idx;              ///< Page index for string storage.
    mutable char* _buf;         ///< Cached character buffer; refreshed on each access to avoid dangling.
    size_type _size;            ///< Current string length.
    size_type _capacity;        ///< Usable character capacity (excl. null).

    /**
     * @brief Allocate initial page and setup internal state.
     * @param min_capacity Required capacity hint (within single page).
     */
    void allocate_initial_page(size_type /*min_capacity*/) {
        int pidx;
        VMManager::instance().alloc_page(&pidx, true);
        _page_idx = pidx;
        // Initialize capacity; pointer will be obtained lazily on access.
        _capacity = VMManager::instance().get_page_size() - 1;
        _buf = nullptr; // will be acquired via write_buf/read_buf
    }

    /**
     * @brief Reallocate to a fresh page (still single page) and copy existing data.
     * @param min_capacity Required capacity.
     */
    void reallocate_page(size_type min_capacity) {
        if (min_capacity > max_size()) throw std::length_error("VMString::reallocate_page exceeds single page limit");
        // Acquire read pointer to existing data before switching pages
        const char* old_buf = (_page_idx >= 0) ? read_buf() : nullptr;

        int new_page_idx;
        VMManager::instance().alloc_page(&new_page_idx, true);
        char* new_buf = reinterpret_cast<char*>(VMManager::instance().get_write_ptr(new_page_idx, 0));
        size_type new_capacity = VMManager::instance().get_page_size() - 1;
        size_type copy_len = std::min(_size, new_capacity);
        if (copy_len && old_buf) memcpy(new_buf, old_buf, copy_len);
        _size = copy_len;
        new_buf[_size] = '\0';
        if (_page_idx >= 0)
            VMManager::instance().free_page(_page_idx);
        _page_idx = new_page_idx;
        _buf = new_buf; // update cache to the new resident buffer
        _capacity = new_capacity;
    }

    /**
     * @brief Ensure capacity for at least min_capacity bytes (including null).
     * @param min_capacity Minimum required including terminator.
     */
    void ensure_capacity(size_type min_capacity) {
        if (min_capacity - 1 > max_size())
            throw std::length_error("VMString exceeds single page capacity");
        if (min_capacity - 1 > _capacity)
            reallocate_page(min_capacity - 1);
    }

    /**
     * @brief Acquire writable buffer pointer and update cache.
     * @return Writable buffer pointer.
     * @throws std::runtime_error If page is not available.
     */
    char* write_buf() const {
        if (_page_idx < 0) return const_cast<char*>(""); // moved-from; shouldn't happen for active strings
        char* p = reinterpret_cast<char*>(VMManager::instance().get_write_ptr(_page_idx, 0));
        if (!p) throw std::runtime_error("VMString: failed to acquire write buffer");
        _buf = p;
        return p;
    }

    /**
     * @brief Acquire read-only buffer pointer and update cache.
     * @return Read-only buffer pointer (never null; empty string if unavailable).
     */
    const char* read_buf() const {
        if (_page_idx < 0) return "";
        char* p = reinterpret_cast<char*>(VMManager::instance().get_read_ptr(_page_idx, 0));
        if (!p) return "";
        _buf = p;
        return p;
    }
};

// Concatenation helpers

/**
 * @brief Concatenate two VMStrings.
 * @param a Left operand.
 * @param b Right operand.
 * @return New VMString.
 */
inline VMString operator+(const VMString& a, const VMString& b) {
    VMString r(a.size() + b.size() + 1);
    r.append(a);
    r.append(b);
    return r;
}
/**
 * @brief Concatenate VMString and C-string.
 * @param a Left operand.
 * @param b Right C-string.
 * @return New VMString.
 */
inline VMString operator+(const VMString& a, const char* b) {
    VMString r(a.size() + std::strlen(b) + 1);
    r.append(a);
    r.append(b);
    return r;
}
/**
 * @brief Concatenate C-string and VMString.
 * @param a C-string left.
 * @param b VMString right.
 * @return New VMString.
 */
inline VMString operator+(const char* a, const VMString& b) {
    VMString r(std::strlen(a) + b.size() + 1);
    r.append(a);
    r.append(b);
    return r;
}