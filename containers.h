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
 *  - Small-block heap allocator enabling multiple small objects/arrays to share pages efficiently.
 *
 * Recent improvements:
 *  - VMArray now uses small-heap blocks instead of dedicating entire pages, enabling better memory utilization.
 *  - VMArray automatically calls constructors/destructors for non-trivial types; zero-initializes trivial types.
 *  - VMVector features hybrid mode: starts with flat contiguous storage (enabling data() access) and automatically
 *    transitions to paged mode when size exceeds single-block capacity.
 *  - VMString uses a single page (no dynamic multi-page growth), but now allocates from a shared heap page instead of owning an entire page.
 *  - VMPtr<T> now allocates its object storage from shared heap pages instead of dedicating a whole page.
 *  - VMPtr<T> has a destroy() method for explicit lifetime management.
 *
 * Limitations:
 *  - VMVector data() is only available in flat mode (small vectors); returns nullptr after transition to paged mode.
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
    bool  is_heap;       ///< True if page is managed as a small-block heap page.
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
 *  - Providing a small-block heap allocator spread across dedicated "heap pages"
 *    used by VMPtr<T> and VMString, so they no longer monopolize a whole page.
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
     * @note Portability: avoids string mode "r+"; keeps two handles (read/write).
     */
    bool begin(fs::FS& filesystem, const char* swap_path) {
        if (started) end();
        fs = &filesystem;
        fs->remove(swap_path);

        // Open a write handle first. On many Arduino FS, FILE_WRITE implies truncation.
        // We will pre-size the file by writing zeros through this handle, then keep it open.
        swap_write = fs->open(swap_path, FILE_WRITE);
        if (!swap_write) return false;

        // Pre-size the file to the required number of pages by writing zeros.
        uint8_t zero[VM_PAGE_SIZE] = {0};
        for (size_t i = 0; i < page_count; i++) {
            swap_write.seek(i * page_size);
            swap_write.write(zero, page_size);
        }
        swap_write.flush();

        // Open a separate read handle. Keeping both avoids reliance on "r+".
        swap_read = fs->open(swap_path, FILE_READ);
        if (!swap_read) {
            swap_write.close();
            return false;
        }

        // Initialize page table.
        for (size_t i = 0; i < page_count; i++) {
            pages[i].allocated    = false;
            pages[i].in_ram       = false;
            pages[i].can_free_ram = true;
            pages[i].dirty        = false;
            pages[i].zero_filled  = true;
            pages[i].is_heap      = false;
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
        // Flush and close both handles if present.
        if (swap_write) {
            swap_write.flush();
            swap_write.close();
        }
        if (swap_read) {
            swap_read.close();
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
    
    // Friend declaration for make_vm helper function
    template<typename T, typename... Args>
    friend VMPtr<T> make_vm(Args&&... args);

    // -------------------- Private state (hidden from end users) --------------------
    VMPage pages[VM_PAGE_COUNT]; ///< Page table.
    fs::File swap_read;              ///< Read-only handle for the swap file (portable alternative to "r+").
    fs::File swap_write;             ///< Write handle for the swap file (kept open to avoid repeated truncation).
    fs::FS* fs = nullptr;        ///< Filesystem pointer.
    size_t page_size = VM_PAGE_SIZE; ///< Current page size (constant).
    size_t page_count = VM_PAGE_COUNT; ///< Number of pages (constant).

    bool started;                    ///< True if manager initialized.
    uint64_t access_tick;            ///< Global access counter.
    AllocOptions default_alloc_options; ///< Default allocation options.

    // -------------------- Small-block heap (shared pages) --------------------
    /**
     * @brief Internal heap header stored at the start of a heap page.
     */
    struct HeapHeader {
        uint32_t magic;       ///< Magic 'VMHP'.
        uint16_t version;     ///< Format version (1).
        uint16_t reserved;    ///< Reserved.
        uint32_t first_free;  ///< Offset to first free block header (0 if none).
        uint32_t total_free;  ///< Total free bytes in payload area (approximate).
    };

    /**
     * @brief Internal block header stored before each allocated/free block.
     *
     * Layout keeps 8-byte alignment so payloads are naturally aligned.
     */
    struct BlockHeader {
        uint32_t size;        ///< Payload size in bytes (rounded up to alignment).
        uint32_t next_free;   ///< Offset to next free block header (0 if none); valid only when free.
        uint16_t flags;       ///< Bit0 = 1 -> free, 0 -> used.
        uint16_t reserved;    ///< Reserved/padding.
    };

    static constexpr uint32_t HEAP_MAGIC = 0x564D4850u; // 'VMHP'
    static constexpr uint16_t HEAP_VERSION = 1;
    static constexpr size_t   HEAP_ALIGN   = 8;         // 8-byte alignment for payloads
    static constexpr size_t   HH_SIZE      = ((sizeof(HeapHeader) + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1));
    static constexpr size_t   BH_SIZE      = ((sizeof(BlockHeader) + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1));

    /**
     * @brief Align up to HEAP_ALIGN.
     */
    static size_t align_up(size_t v) {
        return (v + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1);
    }

    /**
     * @brief Check if page is a heap page (and initialize if needed).
     * @param idx Page index.
     * @return True if page has valid heap header.
     */
    bool ensure_heap_header(int idx) {
        if (!valid_index(idx)) return false;
        VMPage& pg = pages[idx];
        if (!pg.allocated) return false;
        if (!pg.in_ram || !pg.ram_addr) {
            if (!swap_in(idx)) return false;
        }
        HeapHeader* hh = reinterpret_cast<HeapHeader*>(pg.ram_addr);
        if (pg.zero_filled || !pg.is_heap || hh->magic != HEAP_MAGIC || hh->version != HEAP_VERSION) {
            // Initialize a new heap header and a single free block.
            memset(pg.ram_addr, 0, page_size);
            hh->magic = HEAP_MAGIC;
            hh->version = HEAP_VERSION;
            hh->reserved = 0;
            size_t first_block_off = HH_SIZE;
            size_t usable = (page_size > first_block_off + BH_SIZE) ? (page_size - first_block_off - BH_SIZE) : 0;
            if (usable == 0) return false;
            BlockHeader* bh = reinterpret_cast<BlockHeader*>(pg.ram_addr + first_block_off);
            bh->size = (uint32_t)align_up(usable);
            bh->next_free = 0;
            bh->flags = 1; // free
            bh->reserved = 0;
            hh->first_free = (uint32_t)first_block_off;
            hh->total_free = (uint32_t)bh->size;
            pg.is_heap = true;
            pg.zero_filled = false;
            pg.dirty = true;
        }
        return true;
    }

    /**
     * @brief Allocate a new heap page (dedicated to small-block allocator).
     * @param out_idx Output page index.
     * @return True on success.
     */
    bool alloc_heap_page(int* out_idx) {
        AllocOptions opts = default_alloc_options;
        opts.zero_on_alloc = true;
        opts.reuse_swap_data = false;
        int idx = -1;
        if (!alloc_page_ex(opts, &idx)) return false;
        pages[idx].is_heap = true;
        if (!ensure_heap_header(idx)) {
            free_page(idx, true);
            return false;
        }
        if (out_idx) *out_idx = idx;
        return true;
    }

    /**
     * @brief Try to allocate a payload block of at least 'size' from any heap page.
     * @param size Requested payload size.
     * @param align Alignment (ignored, we use HEAP_ALIGN globally).
     * @param out_page Output page index.
     * @param out_off Output payload offset in page.
     * @param out_alloc_size Output actual payload size reserved (>= requested).
     * @return True on success.
     */
    bool heap_alloc(size_t size, size_t /*align*/, int* out_page, size_t* out_off, size_t* out_alloc_size) {
        const size_t need = align_up(size);
        // 1) Search existing heap pages
        for (size_t i = 0; i < page_count; ++i) {
            VMPage& pg = pages[i];
            if (!pg.allocated || !pg.is_heap) continue;
            if (!ensure_heap_header((int)i)) continue;
            HeapHeader* hh = reinterpret_cast<HeapHeader*>(pg.ram_addr);
            // Quick filter
            if (hh->total_free < need) continue;

            uint32_t prev_off = 0;
            uint32_t cur_off = hh->first_free;
            while (cur_off) {
                BlockHeader* cur = reinterpret_cast<BlockHeader*>(pg.ram_addr + cur_off);
                if ((cur->flags & 1) && cur->size >= need) {
                    // Found a block; split if large enough to hold another header + 1 byte
                    const size_t remaining = (size_t)cur->size - need;
                    if (remaining >= BH_SIZE + HEAP_ALIGN) {
                        // Split: allocated part stays at cur_off, remainder becomes new free block after it
                        const uint32_t alloc_off = cur_off;
                        const uint32_t new_free_off = alloc_off + (uint32_t)BH_SIZE + (uint32_t)need;
                        BlockHeader* new_free = reinterpret_cast<BlockHeader*>(pg.ram_addr + new_free_off);
                        new_free->size = (uint32_t)align_up(remaining - BH_SIZE);
                        new_free->flags = 1; // free
                        new_free->reserved = 0;
                        // insert new_free into free list in place of cur
                        new_free->next_free = cur->next_free;

                        // Mark current as used
                        cur->size = (uint32_t)need;
                        cur->flags = 0; // used
                        cur->next_free = 0;

                        // Update free list head/prev
                        if (prev_off == 0) {
                            hh->first_free = new_free_off;
                        } else {
                            BlockHeader* prev = reinterpret_cast<BlockHeader*>(pg.ram_addr + prev_off);
                            prev->next_free = new_free_off;
                        }
                        // Update accounting
                        hh->total_free -= (uint32_t)(need + BH_SIZE);
                        pg.dirty = true;

                        if (out_page) *out_page = (int)i;
                        if (out_off) *out_off = alloc_off + BH_SIZE;
                        if (out_alloc_size) *out_alloc_size = need;
                        return true;
                    } else {
                        // Take the whole block without split
                        // Remove from free list
                        if (prev_off == 0) {
                            hh->first_free = cur->next_free;
                        } else {
                            BlockHeader* prev = reinterpret_cast<BlockHeader*>(pg.ram_addr + prev_off);
                            prev->next_free = cur->next_free;
                        }
                        // Mark used
                        cur->flags = 0;
                        uint32_t alloc_size = cur->size;
                        cur->next_free = 0;

                        // Accounting
                        if (hh->total_free >= alloc_size)
                            hh->total_free -= alloc_size;
                        else
                            hh->total_free = 0;
                        pg.dirty = true;

                        if (out_page) *out_page = (int)i;
                        if (out_off) *out_off = cur_off + BH_SIZE;
                        if (out_alloc_size) *out_alloc_size = alloc_size;
                        return true;
                    }
                }
                prev_off = cur_off;
                cur_off = cur->next_free;
            }
        }

        // 2) No fit found -> allocate a new heap page and retry there
        int new_idx = -1;
        if (!alloc_heap_page(&new_idx)) return false;
        VMPage& pg = pages[new_idx];
        if (!ensure_heap_header(new_idx)) return false;
        HeapHeader* hh = reinterpret_cast<HeapHeader*>(pg.ram_addr);
        // Immediately allocate from the single free block
        uint32_t prev_off = 0;
        uint32_t cur_off = hh->first_free;
        while (cur_off) {
            BlockHeader* cur = reinterpret_cast<BlockHeader*>(pg.ram_addr + cur_off);
            if ((cur->flags & 1) && cur->size >= need) {
                // Same split/take logic as above
                const size_t remaining = (size_t)cur->size - need;
                if (remaining >= BH_SIZE + HEAP_ALIGN) {
                    const uint32_t alloc_off = cur_off;
                    const uint32_t new_free_off = alloc_off + (uint32_t)BH_SIZE + (uint32_t)need;
                    BlockHeader* new_free = reinterpret_cast<BlockHeader*>(pg.ram_addr + new_free_off);
                    new_free->size = (uint32_t)align_up(remaining - BH_SIZE);
                    new_free->flags = 1;
                    new_free->reserved = 0;
                    new_free->next_free = cur->next_free;

                    cur->size = (uint32_t)need;
                    cur->flags = 0;
                    cur->next_free = 0;

                    if (prev_off == 0) {
                        hh->first_free = new_free_off;
                    } else {
                        BlockHeader* prev = reinterpret_cast<BlockHeader*>(pg.ram_addr + prev_off);
                        prev->next_free = new_free_off;
                    }
                    hh->total_free -= (uint32_t)(need + BH_SIZE);
                    pg.dirty = true;

                    if (out_page) *out_page = new_idx;
                    if (out_off) *out_off = alloc_off + BH_SIZE;
                    if (out_alloc_size) *out_alloc_size = need;
                    return true;
                } else {
                    if (prev_off == 0) {
                        hh->first_free = cur->next_free;
                    } else {
                        BlockHeader* prev = reinterpret_cast<BlockHeader*>(pg.ram_addr + prev_off);
                        prev->next_free = cur->next_free;
                    }
                    cur->flags = 0;
                    uint32_t alloc_size = cur->size;
                    cur->next_free = 0;

                    if (hh->total_free >= alloc_size)
                        hh->total_free -= alloc_size;
                    else
                        hh->total_free = 0;
                    pg.dirty = true;

                    if (out_page) *out_page = new_idx;
                    if (out_off) *out_off = cur_off + BH_SIZE;
                    if (out_alloc_size) *out_alloc_size = alloc_size;
                    return true;
                }
            }
            prev_off = cur_off;
            cur_off = cur->next_free;
        }
        return false;
    }

    /**
     * @brief Free a previously allocated small block by payload offset.
     * @param page_idx Page index the block resides in.
     * @param payload_off Offset to payload (not header).
     */
    void heap_free(int page_idx, size_t payload_off) {
        if (!valid_index(page_idx)) return;
        VMPage& pg = pages[page_idx];
        if (!pg.allocated || !pg.is_heap) return;
        if (!ensure_heap_header(page_idx)) return;
        if (payload_off < BH_SIZE) return;
        size_t hdr_off = payload_off - BH_SIZE;
        if (hdr_off + BH_SIZE > page_size) return;

        HeapHeader* hh = reinterpret_cast<HeapHeader*>(pg.ram_addr);
        BlockHeader* bh = reinterpret_cast<BlockHeader*>(pg.ram_addr + hdr_off);

        // Basic sanity
        if ((bh->flags & 1) == 0) {
            // Mark as free and push to free list head (no coalescing to keep it simple)
            bh->flags = 1;
            bh->next_free = hh->first_free;
            hh->first_free = (uint32_t)hdr_off;
            hh->total_free += bh->size;
            pg.dirty = true;
        }
    }

    /**
     * @brief Theoretical maximum payload size for a single small block within one page.
     * @return Max payload bytes.
     */
    size_t heap_max_payload() const {
        // one header and one block header overhead
        if (page_size <= HH_SIZE + BH_SIZE) return 0;
        return page_size - HH_SIZE - BH_SIZE;
    }

    // -------------------- Private helpers (used by friends) --------------------

    /**
     * @brief Evict one RAM-resident page using an LRU policy.
     * @return True if a page was evicted (RAM freed), false otherwise.
     *
     * @details
     * Chooses among pages that are allocated, currently resident in RAM (in_ram && ram_addr),
     * and permitted to free RAM (can_free_ram). The victim is the page with the smallest
     * last_access value (least recently used). Dirty pages are flushed via swap_out().
     * Returns false if no eligible page exists for eviction.
     */
    bool evict_one_page() {
        int victim = -1;
        uint64_t best = std::numeric_limits<uint64_t>::max();

        for (int i = 0; i < (int)page_count; ++i) {
            VMPage& pg = pages[i];
            if (!pg.allocated) continue;
            if (!pg.in_ram || !pg.ram_addr) continue;
            if (!pg.can_free_ram) continue;
            // Pick the least recently accessed page
            if (pg.last_access < best) {
                best = pg.last_access;
                victim = i;
            }
        }
        if (victim < 0) return false;
        // swap_out() flushes dirty pages and frees RAM. Returns true on success.
        return swap_out(victim, false);
    }

    /**
     * @brief Allocate a page-sized RAM buffer; if malloc fails, evict pages until it succeeds.
     * @return Pointer to allocated buffer, or nullptr if eviction did not free enough RAM.
     *
     * @details
     * Repeatedly tries malloc(page_size). On failure, evicts one LRU page and retries.
     * Attempts are bounded by page_count to avoid unbounded loops. If evict_one_page()
     * returns false (no eligible page to evict), the loop terminates early.
     */
    uint8_t* alloc_ram_buffer_with_eviction() {
        for (size_t attempt = 0; attempt < page_count; ++attempt) {
            uint8_t* p = static_cast<uint8_t*>(malloc(page_size));
            if (p) return p;
            if (!evict_one_page()) break;
        }
        return nullptr;
    }

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
                // Allocate RAM buffer with eviction fallback
                pg.ram_addr = alloc_ram_buffer_with_eviction();
                if (!pg.ram_addr) return nullptr;
                pg.allocated    = true;
                pg.in_ram       = true;
                pg.can_free_ram = opts.can_free_ram;
                pg.last_access  = ++access_tick;
                pg.is_heap      = false;

                if (opts.reuse_swap_data) {
                    // Read existing content from swap through the read handle.
                    swap_read.seek(pg.swap_offset);
                    swap_read.read(pg.ram_addr, page_size);
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
        // Allocate RAM buffer with eviction fallback
        pg.ram_addr = alloc_ram_buffer_with_eviction();
        if (!pg.ram_addr) return nullptr;
        pg.allocated    = true;
        pg.in_ram       = true;
        pg.can_free_ram = opts.can_free_ram;
        pg.last_access  = ++access_tick;
        pg.is_heap      = false;

        if (opts.reuse_swap_data) {
            swap_read.seek(pg.swap_offset);
            swap_read.read(pg.ram_addr, page_size);
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
            swap_write.seek(page.swap_offset);
            size_t written = swap_write.write(page.ram_addr, page_size);
            swap_write.flush();
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
            // Allocate RAM buffer with eviction fallback
            page.ram_addr = alloc_ram_buffer_with_eviction();
            if (!page.ram_addr) return false;
            page.in_ram = true;
        }
        swap_read.seek(page.swap_offset);
        size_t readed = swap_read.read(page.ram_addr, page_size);
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
            swap_write.seek(page.swap_offset);
            swap_write.write(zero, page_size);
            swap_write.flush();
        }

        if (page.ram_addr) {
            free(page.ram_addr);
            page.ram_addr = nullptr;
        }
        page.in_ram = false;
        page.allocated = false;
        page.dirty = false;
        page.zero_filled = true;
        page.is_heap = false;
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

    // -------------------- Private allocator wrappers (page-level) --------------------

    /**
     * @brief Allocate a page with options (wrapper over alloc_page_ex).
     * @param out_idx Output page index.
     * @param opts Allocation options.
     * @return True on success, false on failure.
     */
    bool page_alloc(int& out_idx, const AllocOptions& opts) {
        int idx = -1;
        uint8_t* ptr = alloc_page_ex(opts, &idx);
        if (ptr) {
            out_idx = idx;
            return true;
        }
        return false;
    }

    /**
     * @brief Allocate a page with default options (wrapper over alloc_page_ex).
     * @param out_idx Output page index.
     * @return True on success, false on failure.
     */
    bool page_alloc(int& out_idx) {
        return page_alloc(out_idx, default_alloc_options);
    }

    /**
     * @brief Free a page (wrapper over free_page).
     * @param idx Page index.
     * @param wipe If true, overwrite swap content with zeros.
     * @return True on success.
     */
    bool page_free(int idx, bool wipe = false) {
        return free_page(idx, wipe);
    }

    /**
     * @brief Get read-only pointer to page data (wrapper over get_read_ptr).
     * @param idx Page index.
     * @param offset Offset in bytes.
     * @return Pointer or nullptr.
     */
    void* page_read_ptr(int idx, size_t offset) {
        return get_read_ptr(idx, offset);
    }

    /**
     * @brief Get writable pointer to page data (wrapper over get_write_ptr).
     * @param idx Page index.
     * @param offset Offset in bytes.
     * @return Pointer or nullptr.
     */
    void* page_write_ptr(int idx, size_t offset) {
        return get_write_ptr(idx, offset);
    }

    /**
     * @brief Flush a page to disk (wrapper over flush_page).
     * @param idx Page index.
     * @return True on success.
     */
    bool page_flush(int idx) {
        return flush_page(idx);
    }

    /**
     * @brief Prefetch/swap-in a page (wrapper over swap_in).
     * @param idx Page index.
     * @return True on success.
     */
    bool page_prefetch(int idx) {
        return swap_in(idx);
    }

    // -------------------- Private allocator wrappers (small-block) --------------------

    /**
     * @brief Allocate a small block from heap pages (wrapper over heap_alloc).
     * @param size Requested payload size.
     * @param align Alignment (passed through but may be ignored by heap_alloc).
     * @param out_page Output page index.
     * @param out_off Output payload offset.
     * @param out_alloc_size Output actual allocated size.
     * @return True on success.
     */
    bool small_alloc(size_t size, size_t align, int& out_page, size_t& out_off, size_t& out_alloc_size) {
        int pg = -1;
        size_t off = 0;
        size_t sz = 0;
        if (heap_alloc(size, align, &pg, &off, &sz)) {
            out_page = pg;
            out_off = off;
            out_alloc_size = sz;
            return true;
        }
        return false;
    }

    /**
     * @brief Free a small block (wrapper over heap_free).
     * @param page_idx Page index.
     * @param payload_off Payload offset.
     */
    void small_free(int page_idx, size_t payload_off) {
        heap_free(page_idx, payload_off);
    }

    /**
     * @brief Get read-only pointer to small-block payload.
     * @param page_idx Page index.
     * @param payload_off Payload offset.
     * @return Pointer or nullptr.
     */
    void* small_read_ptr(int page_idx, size_t payload_off) {
        return get_read_ptr(page_idx, payload_off);
    }

    /**
     * @brief Get writable pointer to small-block payload.
     * @param page_idx Page index.
     * @param payload_off Payload offset.
     * @return Pointer or nullptr.
     */
    void* small_write_ptr(int page_idx, size_t payload_off) {
        return get_write_ptr(page_idx, payload_off);
    }

    /**
     * @brief Reallocate a small block by allocating new, copying, and freeing old.
     * @param old_page Old page index.
     * @param old_off Old payload offset.
     * @param new_min_size New minimum size required.
     * @param new_page Output new page index.
     * @param new_off Output new payload offset.
     * @param new_alloc_size Output new allocated size.
     * @param copy_bytes Number of bytes to copy from old to new.
     * @return True on success.
     */
    bool small_realloc_move(int old_page, size_t old_off, size_t new_min_size,
                            int& new_page, size_t& new_off, size_t& new_alloc_size,
                            size_t copy_bytes) {
        // Allocate new block
        int np = -1;
        size_t noff = 0;
        size_t nsize = 0;
        if (!small_alloc(new_min_size, 1, np, noff, nsize)) {
            return false;
        }
        // Copy data from old to new
        size_t to_copy = std::min(copy_bytes, nsize);
        if (to_copy > 0) {
            void* old_ptr = small_read_ptr(old_page, old_off);
            void* new_ptr = small_write_ptr(np, noff);
            if (old_ptr && new_ptr) {
                memcpy(new_ptr, old_ptr, to_copy);
            }
        }
        // Free old block
        small_free(old_page, old_off);
        // Return new allocation info
        new_page = np;
        new_off = noff;
        new_alloc_size = nsize;
        return true;
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
 *  - Small-block allocation: on first use, storage is allocated from the manager's shared heap pages
 *    (instead of dedicating a whole page). Multiple VMPtr objects share the same heap pages.
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
        if (page_idx_ == -1) return true; // lazy unallocated is valid
        return mgr.valid_index(page_idx_)
            && offset_ + sizeof(T) <= mgr.get_page_size();
    }

    /**
     * @brief Dereference pointer for write access, ensures the page is loaded, returns reference to object.
     * @return Reference to object.
     * @throws std::runtime_error if invalid or on swap/ptr acquisition failure.
     */
    T& operator*() {
        ensure_loaded();
        return *ptr_write();
    }
    /**
     * @brief Dereference pointer for read-only access (does not mark page dirty).
     * @return Const reference to object.
     * @throws std::runtime_error if invalid or on swap/ptr acquisition failure.
     */
    const T& operator*() const {
        ensure_loaded();
        return *ptr_read();
    }

    /**
     * @brief Member access operator for write access.
     * @return Pointer to object.
     * @throws std::runtime_error if invalid or on swap/ptr acquisition failure.
     */
    T* operator->() {
        ensure_loaded();
        return ptr_write();
    }
    /**
     * @brief Member access operator for read-only access.
     * @return Const pointer to object.
     * @throws std::runtime_error if invalid or on swap/ptr acquisition failure.
     */
    const T* operator->() const {
        ensure_loaded();
        return ptr_read();
    }

    /**
     * @brief Get raw pointer to object in RAM (write intent; marks page dirty).
     * @return Pointer to object.
     * @throws std::runtime_error if invalid or on swap/ptr acquisition failure.
     */
    T* get() {
        ensure_loaded();
        return ptr_write();
    }
    /**
     * @brief Get const raw pointer to object in RAM (read-only; does not mark page dirty).
     * @return Const pointer to object.
     * @throws std::runtime_error if invalid or on swap/ptr acquisition failure.
     */
    const T* get() const {
        ensure_loaded();
        return ptr_read();
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
     * @brief Destroy the managed object and free its VM storage.
     *
     * @details
     * For non-trivial types (types with user-defined destructors), this method explicitly
     * calls the destructor on the managed object before freeing the VM storage.
     * For trivial types, only the storage is freed without calling a destructor.
     * 
     * After calling destroy(), the VMPtr becomes null (page_idx_ = -1) and should not be
     * dereferenced or used to access the object. Calling destroy() on a null VMPtr is safe
     * and does nothing.
     * 
     * This method provides explicit lifetime management for objects stored in virtual memory,
     * similar to std::unique_ptr::reset() or manual delete.
     * 
     * @note This method does not automatically get called when VMPtr goes out of scope.
     *       Users must explicitly call destroy() when they want to end the object's lifetime.
     * 
     * Example usage:
     * @code
     * auto ptr = make_vm<MyClass>(arg1, arg2);
     * ptr->method();  // Use the object
     * ptr.destroy();  // Explicitly destroy and free
     * // ptr is now null and should not be used
     * @endcode
     */
    void destroy() {
        if (page_idx_ < 0) return; // Already null, nothing to do
        
        // For non-trivial types, explicitly call destructor
        if (!std::is_trivially_destructible<T>::value) {
            try {
                // Ensure loaded so we can call destructor
                ensure_loaded();
                T* obj = ptr_write();
                obj->~T();
            } catch (...) {
                // If ensure_loaded or ptr_write fails, we still need to free storage
                // Continue to free even if destructor call failed
            }
        }
        
        // Free the VM storage
        VMManager::instance().small_free(page_idx_, offset_);
        
        // Mark as null
        page_idx_ = -1;
        offset_ = 0;
    }

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
     * @brief Pointer arithmetic: addition (move forward/backward n elements).
     * @param n Number of elements (can be negative).
     * @return New VMPtr<T> advanced by n elements.
     * @throws std::runtime_error if called on an invalid pointer.
     *
     * @details Uses signed arithmetic with floor-division semantics to avoid unsigned wraparounds.
     */
    VMPtr operator+(ptrdiff_t n) const {
        if (!valid()) throw std::runtime_error("VMPtr: arithmetic on invalid pointer");
        const auto& mgr = VMManager::instance();
        const int64_t ps = static_cast<int64_t>(mgr.get_page_size());
        const int64_t total = static_cast<int64_t>(offset_) + static_cast<int64_t>(n) * static_cast<int64_t>(sizeof(T));
        int64_t page_delta = total / ps;
        int64_t rem = total % ps;
        if (rem < 0) { rem += ps; page_delta -= 1; }
        int new_page = page_idx_ + static_cast<int>(page_delta);
        size_t new_offset = static_cast<size_t>(rem);
        return VMPtr(new_page, new_offset);
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
     * @brief Indexing operator: get reference to element at offset n (write intent for non-const).
     * @param n Index offset (can be negative).
     * @return Reference to element at offset.
     */
    T& operator[](ptrdiff_t n) {
        return *(*this + n);
    }
    /**
     * @brief Indexing operator: get const reference to element at offset n (read-only).
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

    // Friend declaration for make_vm helper function
    template<typename U, typename... Args>
    friend VMPtr<U> make_vm(Args&&... args);

private:
    /**
     * @brief Ensure the referenced storage is ready: small-block allocate if needed and load into RAM if not resident.
     *
     * @throws std::runtime_error If allocation/swap-in fails.
     */
    void ensure_loaded() const {
        auto& mgr = VMManager::instance();
        // Allocate from shared heap on first-time use.
        if (page_idx_ == -1) {
            int new_idx = -1;
            size_t new_off = 0;
            size_t alloc_sz = 0;
            if (!mgr.small_alloc(sizeof(T), alignof(T), new_idx, new_off, alloc_sz))
                throw std::runtime_error("VMPtr: failed to heap-allocate storage");
            page_idx_ = new_idx;
            offset_   = new_off;
        } else {
            if (!mgr.valid_index(page_idx_))
                throw std::runtime_error("VMPtr: page index out of range");
        }

        // Ensure the object fits entirely inside a single page.
        if (offset_ + sizeof(T) > mgr.get_page_size())
            throw std::runtime_error("VMPtr: object straddles page boundary");

        // Load into RAM only if not resident.
        if (!mgr.pages[page_idx_].in_ram || !mgr.pages[page_idx_].ram_addr) {
            if (!mgr.page_prefetch(page_idx_))
                throw std::runtime_error("VMPtr: failed to swap-in page");
        }
    }

    /**
     * @brief Acquire writable pointer to the object (marks page dirty).
     * @return Writable pointer to object (never null on success).
     * @throws std::runtime_error If pointer acquisition fails.
     */
    T* ptr_write() const {
        T* p = reinterpret_cast<T*>(VMManager::instance().small_write_ptr(page_idx_, offset_));
        if (!p) throw std::runtime_error("VMPtr: failed to acquire write pointer");
        return p;
    }

    /**
     * @brief Acquire read-only pointer to the object (does not mark page dirty).
     * @return Read-only pointer to object (never null on success).
     * @throws std::runtime_error If pointer acquisition fails.
     */
    const T* ptr_read() const {
        const T* p = reinterpret_cast<const T*>(VMManager::instance().small_read_ptr(page_idx_, offset_));
        if (!p) throw std::runtime_error("VMPtr: failed to acquire read pointer");
        return p;
    }

    mutable int page_idx_;   ///< Index of page in VMManager (heap-allocated on demand).
    mutable size_t offset_;  ///< Offset inside the page (in bytes) to payload.
};

// -----------------------------------------------------------------------------
// VMPtr helper functions
// -----------------------------------------------------------------------------

/**
 * @brief Factory function to create and construct a VMPtr-managed object.
 * @tparam T Object type.
 * @tparam Args Constructor argument types.
 * @param args Constructor arguments.
 * @return VMPtr<T> pointing to newly constructed object.
 * @throws std::runtime_error If allocation or construction fails.
 *
 * @details
 * Creates a VMPtr<T> and constructs the object in-place using the provided arguments.
 * This provides a safer, smart-pointer-like workflow similar to std::make_unique.
 * The object is allocated from VMManager's shared heap pages and constructed using
 * placement new with perfect forwarding of arguments.
 *
 * Benefits over manual construction:
 *  - Exception-safe: automatically frees allocated memory if constructor throws
 *  - Cleaner syntax: no need to manually handle allocation/construction steps
 *  - Type-safe: uses perfect forwarding for constructor arguments
 *
 * Example usage:
 * @code
 * // Use make_vm for direct construction:
 * auto ptr = make_vm<MyClass>(arg1, arg2, arg3);
 * ptr->method();
 * 
 * // Works with any constructor:
 * auto defaultPtr = make_vm<MyClass>();              // Default constructor
 * auto singleArg = make_vm<MyClass>(42);             // Single argument
 * auto multiArg = make_vm<MyClass>(1, "test", 3.14); // Multiple arguments
 * @endcode
 *
 * @note The returned VMPtr does not automatically call the destructor when it goes out of scope.
 *       This is consistent with VMPtr's design where users manage object lifetime explicitly.
 */
template<typename T, typename... Args>
VMPtr<T> make_vm(Args&&... args) {
    auto& mgr = VMManager::instance();
    
    // Allocate storage from small heap
    int page_idx = -1;
    size_t offset = 0;
    size_t alloc_sz = 0;
    if (!mgr.small_alloc(sizeof(T), alignof(T), page_idx, offset, alloc_sz)) {
        throw std::runtime_error("make_vm: failed to allocate storage");
    }
    
    // Get writable pointer to the allocated space
    void* ptr = mgr.small_write_ptr(page_idx, offset);
    if (!ptr) {
        mgr.small_free(page_idx, offset);
        throw std::runtime_error("make_vm: failed to acquire write pointer");
    }
    
    // Construct object in-place using placement new with perfect forwarding
    try {
        new(ptr) T(std::forward<Args>(args)...);
    } catch (...) {
        mgr.small_free(page_idx, offset);
        throw;
    }
    
    return VMPtr<T>(page_idx, offset);
}

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
 * @brief Vector-like container backed by pageable storage with hybrid mode.
 * @tparam T Element type.
 * @note Not fully STL-compatible; no allocator support. Iterators become invalid
 *       after modifications similar to std::vector in some operations.
 * @details
 * Hybrid mode: Small vectors start in "flat" mode using a single contiguous heap block,
 * enabling data() access. When size exceeds flat capacity, transitions to "paged" mode
 * spanning multiple pages (data() becomes unavailable).
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

    /// Default constructor (starts in flat mode).
    VMVector() : _chunk_capacity(VM_PAGE_SIZE / sizeof(T)), _chunk_count(0), _size(0),
                 _flat_mode(true), _flat_page(-1), _flat_offset(0), _flat_capacity(0) {
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
        : _chunk_capacity(other._chunk_capacity), _chunk_count(other._chunk_count), _size(other._size),
          _flat_mode(other._flat_mode), _flat_page(other._flat_page), 
          _flat_offset(other._flat_offset), _flat_capacity(other._flat_capacity) {
        for (size_type i = 0; i < VM_PAGE_COUNT; ++i) {
            _chunks[i] = other._chunks[i];
            other._chunks[i].page_idx = -1;
            other._chunks[i].count = 0;
        }
        other._chunk_count = 0;
        other._size = 0;
        other._flat_mode = true;
        other._flat_page = -1;
        other._flat_offset = 0;
        other._flat_capacity = 0;
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
            _flat_mode      = other._flat_mode;
            _flat_page      = other._flat_page;
            _flat_offset    = other._flat_offset;
            _flat_capacity  = other._flat_capacity;
            for (size_type i = 0; i < VM_PAGE_COUNT; ++i) {
                _chunks[i] = other._chunks[i];
                other._chunks[i].page_idx = -1;
                other._chunks[i].count = 0;
            }
            other._size = 0;
            other._chunk_count = 0;
            other._flat_mode = true;
            other._flat_page = -1;
            other._flat_offset = 0;
            other._flat_capacity = 0;
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
        if (_flat_mode) {
            T* base = reinterpret_cast<T*>(VMManager::instance().small_write_ptr(_flat_page, _flat_offset));
            return base[idx];
        } else {
            size_type chunk_num = idx / _chunk_capacity;
            size_type offset    = idx % _chunk_capacity;
            Chunk& ch = _chunks[chunk_num];
            return *reinterpret_cast<T*>(VMManager::instance().page_write_ptr(ch.page_idx, offset * sizeof(T)));
        }
    }
    /**
     * @brief Unchecked element access (read intent).
     * @param idx Element index.
     * @return Const reference.
     */
    const_reference operator[](size_type idx) const {
        if (_flat_mode) {
            const T* base = reinterpret_cast<const T*>(VMManager::instance().small_read_ptr(_flat_page, _flat_offset));
            return base[idx];
        } else {
            size_type chunk_num = idx / _chunk_capacity;
            size_type offset    = idx % _chunk_capacity;
            const Chunk& ch = _chunks[chunk_num];
            return *reinterpret_cast<const T*>(VMManager::instance().page_read_ptr(ch.page_idx, offset * sizeof(T)));
        }
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
    /// Current capacity in elements (sum of allocated chunks or flat capacity).
    size_type capacity() const { 
        return _flat_mode ? _flat_capacity : (_chunk_count * _chunk_capacity); 
    }

    /**
     * @brief Check if vector is in flat contiguous mode.
     * @return True if in flat mode (data() is available).
     */
    bool is_flat() const { return _flat_mode; }

    /**
     * @brief Get pointer to contiguous data (only available in flat mode).
     * @return Pointer to data, or nullptr if not in flat mode.
     * @note Only valid while vector remains in flat mode. Operations that
     *       cause transition to paged mode will invalidate this pointer.
     */
    T* data() {
        if (!_flat_mode || _flat_page < 0) return nullptr;
        return reinterpret_cast<T*>(VMManager::instance().small_write_ptr(_flat_page, _flat_offset));
    }

    /**
     * @brief Get const pointer to contiguous data (only available in flat mode).
     * @return Const pointer to data, or nullptr if not in flat mode.
     */
    const T* data() const {
        if (!_flat_mode || _flat_page < 0) return nullptr;
        return reinterpret_cast<const T*>(VMManager::instance().small_read_ptr(_flat_page, _flat_offset));
    }

    /**
     * @brief Append element by copy.
     * @param value Value to copy.
     */
    void push_back(const T& value) {
        if (_flat_mode) {
            ensure_flat_back_slot();
            if (_flat_mode) {
                // Still in flat mode
                T* base = reinterpret_cast<T*>(VMManager::instance().small_write_ptr(_flat_page, _flat_offset));
                new(&base[_size]) T(value);
                _size++;
                return;
            }
        }
        // Paged mode (or transitioned to paged)
        ensure_back_slot();
        Chunk& ch = _chunks[_chunk_count - 1];
        T* ptr = reinterpret_cast<T*>(VMManager::instance().page_write_ptr(ch.page_idx, ch.count * sizeof(T)));
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
        if (_flat_mode) {
            ensure_flat_back_slot();
            if (_flat_mode) {
                // Still in flat mode
                T* base = reinterpret_cast<T*>(VMManager::instance().small_write_ptr(_flat_page, _flat_offset));
                new(&base[_size]) T(std::forward<Args>(args)...);
                _size++;
                return base[_size - 1];
            }
        }
        // Paged mode (or transitioned to paged)
        ensure_back_slot();
        Chunk& ch = _chunks[_chunk_count - 1];
        T* ptr = reinterpret_cast<T*>(VMManager::instance().page_write_ptr(ch.page_idx, ch.count * sizeof(T)));
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
        if (_flat_mode) {
            T* base = reinterpret_cast<T*>(VMManager::instance().small_write_ptr(_flat_page, _flat_offset));
            base[_size - 1].~T();
            _size--;
            return;
        }
        // Paged mode
        _size--;
        size_type chunk_num = _size / _chunk_capacity;
        size_type offset    = _size % _chunk_capacity;
        if (offset == 0 && chunk_num > 0) chunk_num--;
        Chunk& ch = _chunks[chunk_num];
        T* ptr = reinterpret_cast<T*>(VMManager::instance().page_write_ptr(ch.page_idx, (ch.count - 1) * sizeof(T)));
        ptr->~T();
        ch.count--;
        if (ch.count == 0) {
            VMManager::instance().page_free(ch.page_idx);
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
        if (_flat_mode) {
            // Destroy elements in flat mode
            if (_flat_page >= 0 && _size > 0) {
                T* base = reinterpret_cast<T*>(VMManager::instance().small_write_ptr(_flat_page, _flat_offset));
                for (size_type i = 0; i < _size; ++i) {
                    base[i].~T();
                }
                VMManager::instance().small_free(_flat_page, _flat_offset);
                _flat_page = -1;
                _flat_offset = 0;
                _flat_capacity = 0;
            }
        } else {
            // Original paged mode cleanup
            for (size_type i = 0; i < _chunk_count; ++i) {
                Chunk& ch = _chunks[i];
                if (ch.page_idx == -1) continue;
                for (size_type j = 0; j < ch.count; ++j) {
                    T* ptr = reinterpret_cast<T*>(VMManager::instance().page_write_ptr(ch.page_idx, j * sizeof(T)));
                    ptr->~T();
                }
                VMManager::instance().page_free(ch.page_idx);
                ch.page_idx = -1;
                ch.count = 0;
            }
            _chunk_count = 0;
        }
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
            int page_idx = -1;
            VMManager::AllocOptions opts;
            opts.can_free_ram = true;
            opts.zero_on_alloc = true;
            opts.reuse_swap_data = false;
            VMManager::instance().page_alloc(page_idx, opts);
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
                VMManager::instance().page_free(_chunks[i].page_idx);
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
    size_type _chunk_capacity;    ///< Elements per chunk.
    size_type _chunk_count;       ///< Active chunk count.
    size_type _size;              ///< Total elements.
    
    // Flat mode members
    bool _flat_mode;              ///< True if using contiguous flat block.
    int _flat_page;               ///< Page index for flat block.
    size_t _flat_offset;          ///< Offset within page for flat block.
    size_type _flat_capacity;     ///< Capacity in elements for flat block.

    /**
     * @brief Ensure space for one more element in flat mode; transition to paged if needed.
     */
    void ensure_flat_back_slot() {
        // First-time allocation in flat mode
        if (_flat_page < 0) {
            // Start with a reasonable initial capacity
            size_t initial_cap = 16; // elements
            if (initial_cap * sizeof(T) > VMManager::instance().heap_max_payload()) {
                initial_cap = VMManager::instance().heap_max_payload() / sizeof(T);
            }
            if (initial_cap < 1) initial_cap = 1;
            
            size_t needed = initial_cap * sizeof(T);
            size_t alloc_sz = 0;
            if (VMManager::instance().small_alloc(needed, alignof(T), _flat_page, _flat_offset, alloc_sz)) {
                _flat_capacity = alloc_sz / sizeof(T);
                return;
            }
            // If small_alloc fails, transition to paged mode immediately
            transition_to_paged();
            return;
        }
        
        // Check if we have space
        if (_size < _flat_capacity) {
            return; // Space available
        }
        
        // Try to grow the flat buffer
        size_t new_cap = _flat_capacity * 2;
        if (new_cap * sizeof(T) > VMManager::instance().heap_max_payload()) {
            // Can't fit in a single heap block, must transition to paged
            transition_to_paged();
            return;
        }
        
        // Try reallocation
        int new_page = -1;
        size_t new_offset = 0;
        size_t new_alloc = 0;
        size_t needed = new_cap * sizeof(T);
        size_t copy_bytes = _size * sizeof(T);
        
        if (VMManager::instance().small_realloc_move(_flat_page, _flat_offset, needed,
                                                      new_page, new_offset, new_alloc, copy_bytes)) {
            _flat_page = new_page;
            _flat_offset = new_offset;
            _flat_capacity = new_alloc / sizeof(T);
            return;
        }
        
        // Reallocation failed, transition to paged mode
        transition_to_paged();
    }
    
    /**
     * @brief Transition from flat mode to paged mode.
     */
    void transition_to_paged() {
        if (!_flat_mode) return;
        
        // Copy existing elements from flat buffer to paged chunks
        if (_size > 0 && _flat_page >= 0) {
            T* flat_base = reinterpret_cast<T*>(VMManager::instance().small_read_ptr(_flat_page, _flat_offset));
            
            // Allocate chunks as needed and copy elements
            for (size_type i = 0; i < _size; ++i) {
                if (_chunk_count == 0 || _chunks[_chunk_count - 1].count >= _chunk_capacity) {
                    int page_idx = -1;
                    VMManager::AllocOptions opts;
                    opts.can_free_ram = true;
                    opts.zero_on_alloc = true;
                    opts.reuse_swap_data = false;
                    VMManager::instance().page_alloc(page_idx, opts);
                    _chunks[_chunk_count].page_idx = page_idx;
                    _chunks[_chunk_count].count = 0;
                    _chunk_count++;
                }
                
                Chunk& ch = _chunks[_chunk_count - 1];
                T* ptr = reinterpret_cast<T*>(VMManager::instance().page_write_ptr(ch.page_idx, ch.count * sizeof(T)));
                new(ptr) T(flat_base[i]); // Copy construct
                ch.count++;
            }
            
            // Free the flat buffer
            VMManager::instance().small_free(_flat_page, _flat_offset);
        }
        
        _flat_mode = false;
        _flat_page = -1;
        _flat_offset = 0;
        _flat_capacity = 0;
    }

    /**
     * @brief Ensure space for one more element, allocate new page if needed (paged mode).
     */
    void ensure_back_slot() {
        if (_chunk_count == 0 || _chunks[_chunk_count - 1].count >= _chunk_capacity) {
            int page_idx = -1;
            VMManager::AllocOptions opts;
            opts.can_free_ram = true;
            opts.zero_on_alloc = true;
            opts.reuse_swap_data = false;
            VMManager::instance().page_alloc(page_idx, opts);
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
 * @brief Fixed-size array backed by a small-heap block.
 * @tparam T Element type.
 * @tparam N Number of elements.
 * @details 
 * Now uses VMManager small-block heap so multiple arrays can share pages efficiently.
 * 
 * Object lifetime management:
 *  - For trivial types (int, POD structs, etc.): memory is zero-initialized, no constructors/destructors called
 *  - For non-trivial types: all elements are default-constructed using placement new in the constructor,
 *    and destructors are called for all elements in the destructor
 * 
 * This ensures VMArray behaves like std::array regarding object lifetime for non-trivial types,
 * while maintaining efficiency for trivial types.
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

    /// Constructor allocates from small-heap blocks.
    VMArray() : page_idx(-1), offset(0) {
        size_t needed = N * sizeof(T);
        size_t alloc_sz = 0;
        if (!VMManager::instance().small_alloc(needed, alignof(T), page_idx, offset, alloc_sz)) {
            throw std::runtime_error("VMArray: small_alloc failed");
        }
        
        void* ptr = VMManager::instance().small_write_ptr(page_idx, offset);
        if (!ptr) {
            VMManager::instance().small_free(page_idx, offset);
            throw std::runtime_error("VMArray: failed to acquire write pointer");
        }
        
        // For trivial types, just zero-initialize the memory
        if (std::is_trivially_default_constructible<T>::value) {
            memset(ptr, 0, alloc_sz);
        } else {
            // For non-trivial types, use placement new to construct each element
            T* arr = reinterpret_cast<T*>(ptr);
            size_t constructed = 0;
            try {
                for (size_t i = 0; i < N; ++i) {
                    new(&arr[i]) T();
                    constructed++;
                }
            } catch (...) {
                // If construction fails, destroy already constructed elements
                for (size_t i = 0; i < constructed; ++i) {
                    arr[i].~T();
                }
                VMManager::instance().small_free(page_idx, offset);
                throw;
            }
        }
    }
    /// Destructor frees heap block.
    ~VMArray() {
        if (page_idx >= 0) {
            // For non-trivial types, explicitly call destructors
            if (!std::is_trivially_destructible<T>::value) {
                void* ptr = VMManager::instance().small_write_ptr(page_idx, offset);
                if (ptr) {
                    T* arr = reinterpret_cast<T*>(ptr);
                    for (size_t i = 0; i < N; ++i) {
                        arr[i].~T();
                    }
                }
            }
            VMManager::instance().small_free(page_idx, offset);
            page_idx = -1;
        }
    }

    /**
     * @brief Unchecked element access (write intent).
     * @param idx Index.
     * @return Reference.
     */
    reference operator[](size_type idx) {
        return *reinterpret_cast<T*>(
            static_cast<uint8_t*>(VMManager::instance().small_write_ptr(page_idx, offset)) + idx * sizeof(T));
    }
    /**
     * @brief Unchecked element access (read intent).
     * @param idx Index.
     * @return Const reference.
     */
    const_reference operator[](size_type idx) const {
        return *reinterpret_cast<const T*>(
            static_cast<const uint8_t*>(VMManager::instance().small_read_ptr(page_idx, offset)) + idx * sizeof(T));
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
     * @brief Reset array elements to default constructed T() and flush page.
     */
    void clear() {
        for (size_type i = 0; i < N; ++i)
            (*this)[i] = T();
        VMManager::instance().page_flush(page_idx);
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
    int page_idx;      ///< Page index in heap page.
    size_t offset;     ///< Offset within the page.
};

// -----------------------------------------------------------------------------
// VMString
// -----------------------------------------------------------------------------

/**
 * @brief Single-page mutable string with limited capacity (allocated from shared heap pages).
 *
 * @details No multi-page growth; exceeding theoretical single-block capacity throws.
 * Now uses VMManager small-block heap so multiple strings/pointers share pages.
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
     * @brief Construct with initial capacity (allocated from heap pages).
     * @param initial_capacity Hint (still limited to single heap-block capacity).
     */
    explicit VMString(size_t initial_capacity = 64)
        : _page_idx(-1), _offset(0), _buf(nullptr), _size(0), _capacity(0) {
        allocate_initial_block(initial_capacity);
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
        : _page_idx(other._page_idx), _offset(other._offset), _buf(other._buf),
          _size(other._size), _capacity(other._capacity) {
        other._page_idx = -1;
        other._offset = 0;
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
            // free current block
            if (_page_idx >= 0) VMManager::instance().small_free(_page_idx, _offset);
            _page_idx = other._page_idx;
            _offset   = other._offset;
            _buf      = other._buf;
            _size     = other._size;
            _capacity = other._capacity;
            other._page_idx = -1;
            other._offset = 0;
            other._buf = nullptr;
            other._size = 0;
            other._capacity = 0;
        }
        return *this;
    }

    /// Destructor frees heap block.
    ~VMString() {
        if (_page_idx >= 0) {
            VMManager::instance().small_free(_page_idx, _offset);
            _buf = nullptr;
            _page_idx = -1;
            _offset = 0;
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
     * @brief Theoretical maximum storable characters in a single heap block (excluding null terminator).
     * @return Limit (page_size - heap_header - block_header - 1).
     */
    size_type max_size() const { return VMManager::instance().heap_max_payload() > 0 ? (VMManager::instance().heap_max_payload() - 1) : 0; }

    /**
     * @brief Ensure capacity >= new_cap.
     * @param new_cap Desired capacity.
     * @throws std::length_error If exceeds single heap-block capacity.
     */
    void reserve(size_type new_cap) {
        if (new_cap <= _capacity) return;
        if (new_cap > max_size()) throw std::length_error("VMString::reserve exceeds max single-block size");
        reallocate_block(new_cap + 1);
    }
    /// No-op (single block).
    void shrink_to_fit() { /* no-op single block */ }

    /**
     * @brief Resize string (fill with ch if expanding).
     * @param new_size New size.
     * @param ch Fill character.
     */
    void resize(size_type new_size, char ch = '\0') {
        if (new_size > max_size()) throw std::length_error("VMString::resize exceeds one heap block");
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
            // Refresh write buffer again in case capacity changed
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
        std::swap(_offset, other._offset);
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
            VMManager::instance().page_flush(_page_idx);
            _buf = nullptr; // avoid stale pointer after potential RAM free
        } else {
            _size = 0;
        }
    }

private:
    int _page_idx;              ///< Page index for string storage (heap page).
    size_t _offset;             ///< Payload offset within the page.
    mutable char* _buf;         ///< Cached character buffer; refreshed on each access to avoid dangling.
    size_type _size;            ///< Current string length.
    size_type _capacity;        ///< Usable character capacity (excl. null).

    /**
     * @brief Allocate initial heap block and setup internal state.
     * @param min_capacity Required capacity hint (within a single heap block).
     */
    void allocate_initial_block(size_type min_capacity) {
        int pidx = -1;
        size_t off = 0;
        size_t alloc_sz = 0;
        size_t need = (min_capacity + 1); // include null
        if (need < 1) need = 1;
        if (need > VMManager::instance().heap_max_payload())
            need = VMManager::instance().heap_max_payload();
        if (!VMManager::instance().small_alloc(need, alignof(char), pidx, off, alloc_sz))
            throw std::runtime_error("VMString: heap_alloc failed");
        _page_idx = pidx;
        _offset = off;
        _capacity = alloc_sz > 0 ? (alloc_sz - 1) : 0; // reserve one byte for null
        _buf = nullptr; // will be acquired via write_buf/read_buf
        _size = 0;
    }

    /**
     * @brief Reallocate to a fresh heap block and copy existing data.
     * @param min_capacity Required capacity (excluding null).
     */
    void reallocate_block(size_type min_capacity) {
        int new_page_idx = -1;
        size_t new_off = 0;
        size_t new_alloc = 0;
        size_t need = min_capacity; // includes null already when called
        if (!VMManager::instance().small_alloc(need, alignof(char), new_page_idx, new_off, new_alloc))
            throw std::length_error("VMString::reserve: cannot allocate requested capacity");
        char* new_buf = reinterpret_cast<char*>(VMManager::instance().small_write_ptr(new_page_idx, new_off));
        size_type copy_len = std::min(_size, new_alloc > 0 ? (new_alloc - 1) : 0);
        if (copy_len) {
            const char* src = read_buf();
            memcpy(new_buf, src, copy_len);
        }
        _size = copy_len;
        new_buf[_size] = '\0';

        // Free old block
        if (_page_idx >= 0) {
            VMManager::instance().small_free(_page_idx, _offset);
        }

        // Update to new location
        _page_idx = new_page_idx;
        _offset = new_off;
        _buf = new_buf; // cache updated
        _capacity = new_alloc > 0 ? (new_alloc - 1) : 0;
    }

    /**
     * @brief Ensure capacity for at least min_capacity bytes (including null).
     * @param min_capacity Minimum required including terminator.
     */
    void ensure_capacity(size_type min_capacity) {
        if (min_capacity - 1 > max_size())
            throw std::length_error("VMString exceeds single block capacity");
        if (min_capacity - 1 > _capacity)
            reallocate_block(min_capacity);
    }

    /**
     * @brief Acquire writable buffer pointer and update cache.
     * @return Writable buffer pointer.
     * @throws std::runtime_error If page is not available.
     */
    char* write_buf() const {
        if (_page_idx < 0) return const_cast<char*>(""); // moved-from; shouldn't happen for active strings
        char* p = reinterpret_cast<char*>(VMManager::instance().small_write_ptr(_page_idx, _offset));
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
        char* p = reinterpret_cast<char*>(VMManager::instance().small_read_ptr(_page_idx, _offset));
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