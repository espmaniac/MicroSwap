#pragma once

/*
 * This code was written using GitHub Copilot.
 *
 * VMManager: virtual memory and swap through an abstract file system (FS, SD, SPIFFS, LittleFS, etc.)
 * Containers: VMVector, VMArray, VMString. The interface is as close as possible to std::vector, std::array, std::string.
 */

#include <FS.h>
#include <initializer_list>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <type_traits>

#define VM_PAGE_SIZE   4096
#define VM_PAGE_COUNT  16

struct VMPage {
    bool in_ram;
    bool can_free_ram;
    uint8_t* ram_addr;
    size_t swap_offset;
};

class VMManager {
public:
    static VMManager& instance() {
        static VMManager inst;
        return inst;
    }

    VMPage pages[VM_PAGE_COUNT];
    File swap_file;
    fs::FS* fs = nullptr;
    size_t page_size = VM_PAGE_SIZE;
    size_t page_count = VM_PAGE_COUNT;

    // Initialize VMManager with a file system and swap file path
    bool begin(fs::FS& filesystem, const char* swap_path) {
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
            pages[i].in_ram = false;
            pages[i].can_free_ram = true;
            pages[i].ram_addr = nullptr;
            pages[i].swap_offset = i * page_size;
        }
        return true;
    }

    // Allocate a page in RAM
    uint8_t* alloc_page(int* out_idx = nullptr, bool can_free_ram = true) {
        for (size_t i = 0; i < page_count; i++) {
            if (!pages[i].in_ram) {
                pages[i].ram_addr = (uint8_t*)malloc(page_size);
                if (!pages[i].ram_addr) return nullptr;
                pages[i].in_ram = true;
                pages[i].can_free_ram = can_free_ram;
                swap_file.seek(pages[i].swap_offset);
                swap_file.read(pages[i].ram_addr, page_size);
                if (out_idx) *out_idx = i;
                return pages[i].ram_addr;
            }
        }
        return nullptr;
    }

    // Swap out a page (save to file and optionally free RAM)
    bool swap_out(int idx) {
        if (idx < 0 || idx >= (int)page_count) return false;
        VMPage& page = pages[idx];
        if (!page.in_ram || !page.ram_addr) return false;
        swap_file.seek(page.swap_offset);
        size_t written = swap_file.write(page.ram_addr, page_size);
        swap_file.flush();
        if (page.can_free_ram) {
            free(page.ram_addr);
            page.ram_addr = nullptr;
            page.in_ram = false;
        }
        return written == page_size;
    }

    // Swap in a page (load from file to RAM)
    bool swap_in(int idx) {
        if (idx < 0 || idx >= (int)page_count) return false;
        VMPage& page = pages[idx];
        if (!page.in_ram || !page.ram_addr) {
            page.ram_addr = (uint8_t*)malloc(page_size);
            if (!page.ram_addr) return false;
            page.in_ram = true;
        }
        swap_file.seek(page.swap_offset);
        size_t readed = swap_file.read(page.ram_addr, page_size);
        return readed == page_size;
    }

    // Get pointer to data at offset in page
    void* get_ptr(int page_idx, size_t offset) {
        if (page_idx < 0 || page_idx >= (int)page_count) return nullptr;
        VMPage& page = pages[page_idx];
        if (!page.in_ram) swap_in(page_idx);
        return page.ram_addr + offset;
    }

    // Cleanup VMManager and free all memory
    void end() {
        for (size_t i = 0; i < page_count; i++) {
            if (pages[i].ram_addr) {
                free(pages[i].ram_addr);
                pages[i].ram_addr = nullptr;
            }
            pages[i].in_ram = false;
        }
        if (swap_file) swap_file.close();
    }

private:
    VMManager() {}
    VMManager(const VMManager&) = delete;
    VMManager& operator=(const VMManager&) = delete;
};


template<typename T>
class VMVector {
public:
    typedef T value_type;
    typedef T& reference;
    typedef const T& const_reference;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef size_t size_type;

    VMVector() : _chunk_capacity(VM_PAGE_SIZE / sizeof(T)), _size(0), _chunk_count(0) {
        for (size_type i = 0; i < VM_PAGE_COUNT; ++i) {
            _chunks[i].page_idx = -1;
            _chunks[i].count = 0;
        }
    }

    VMVector(size_type n, const T& val = T()) : VMVector() { assign(n, val); }

    VMVector(const std::initializer_list<T>& ilist) : VMVector() {
        assign(ilist.begin(), ilist.end());
    }

    ~VMVector() { clear(); }

    // Add element to the end of the vector
    void push_back(const T& value) {
        if (_chunk_count == 0 || _chunks[_chunk_count-1].count >= _chunk_capacity) {
            int page_idx;
            VMManager::instance().alloc_page(&page_idx, true); // allow freeing RAM!
            _chunks[_chunk_count].page_idx = page_idx;
            _chunks[_chunk_count].count = 0;
            _chunk_count++;
        }
        Chunk& ch = _chunks[_chunk_count-1];
        T* ptr = reinterpret_cast<T*>(VMManager::instance().get_ptr(ch.page_idx, ch.count * sizeof(T)));
        new(ptr) T(value);
        ch.count++; _size++;
    }

    // Remove element from the end of the vector
    void pop_back() {
        if (_size == 0) throw std::out_of_range("pop_back from empty VMVector");
        _size--;
        size_type chunk_num = _size / _chunk_capacity;
        size_type offset = _size % _chunk_capacity;
        if (offset == 0 && chunk_num > 0) chunk_num--;
        Chunk& ch = _chunks[chunk_num];
        T* ptr = reinterpret_cast<T*>(VMManager::instance().get_ptr(ch.page_idx, (ch.count-1) * sizeof(T)));
        ptr->~T();
        ch.count--;
        if (ch.count == 0) {
            VMManager::instance().swap_out(ch.page_idx); // free RAM!
            _chunk_count--;
        }
    }

    reference operator[](size_type idx) {
        size_type chunk_num = idx / _chunk_capacity;
        size_type offset = idx % _chunk_capacity;
        Chunk& ch = _chunks[chunk_num];
        return *reinterpret_cast<T*>(VMManager::instance().get_ptr(ch.page_idx, offset * sizeof(T)));
    }

    const_reference operator[](size_type idx) const {
        size_type chunk_num = idx / _chunk_capacity;
        size_type offset = idx % _chunk_capacity;
        const Chunk& ch = _chunks[chunk_num];
        return *reinterpret_cast<const T*>(VMManager::instance().get_ptr(ch.page_idx, offset * sizeof(T)));
    }

    bool empty() const { return _size == 0; }
    size_type size() const { return _size; }
    size_type capacity() const { return _chunk_count * _chunk_capacity; }

    // Clear all elements from the vector
    void clear() {
        for (size_type i = 0; i < _chunk_count; ++i) {
            Chunk& ch = _chunks[i];
            for (size_type j = 0; j < ch.count; ++j) {
                T* ptr = reinterpret_cast<T*>(VMManager::instance().get_ptr(ch.page_idx, j * sizeof(T)));
                ptr->~T();
            }
            VMManager::instance().swap_out(ch.page_idx); // free RAM!
        }
        _chunk_count = 0;
        _size = 0;
    }

    // Resize the vector
    void resize(size_type n, const T& val = T()) {
        if (n < _size) {
            while (_size > n) pop_back();
        } else if (n > _size) {
            while (_size < n) push_back(val);
        }
    }

    // Assign n copies of val to the vector
    void assign(size_type n, const T& val) {
        clear();
        resize(n, val);
    }
    template<typename InputIt>
    void assign(InputIt first, InputIt last) {
        clear();
        for (InputIt it = first; it != last; ++it) push_back(*it);
    }

private:
    struct Chunk {
        int page_idx;
        size_type count;
    };
    Chunk _chunks[VM_PAGE_COUNT];
    size_type _chunk_count;
    size_type _chunk_capacity;
    size_type _size;
};

// VMArray: std::array-like, frees RAM on swap
template<typename T, size_t N>
class VMArray {
public:
    typedef T value_type;
    typedef T& reference;
    typedef const T& const_reference;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef size_t size_type;

    VMArray() {
        VMManager::instance().alloc_page(&page_idx, true);
    }

    reference operator[](size_type idx) {
        return *reinterpret_cast<T*>(VMManager::instance().get_ptr(page_idx, idx * sizeof(T)));
    }
    const_reference operator[](size_type idx) const {
        return *reinterpret_cast<const T*>(VMManager::instance().get_ptr(page_idx, idx * sizeof(T)));
    }

    size_type size() const { return N; }
    bool empty() const { return N == 0; }

    // Fill the array with a value
    void fill(const T& val) {
        for (size_type i = 0; i < N; ++i) (*this)[i] = val;
    }

    // Clear the array and free RAM
    void clear() {
        for (size_type i = 0; i < N; ++i)
            (*this)[i] = T();
        VMManager::instance().swap_out(page_idx); // free RAM!
    }

private:
    int page_idx;
};

class VMString {
public:
    VMString(size_t initial_capacity = 64)
        : _size(0), _capacity(initial_capacity), _page_idx(-1), _buf(nullptr)
    {
        allocate_page(_capacity);
        _buf[0] = '\0';
    }

    VMString(const char* s) : VMString(strlen(s) + 1) {
        assign(s);
    }

    VMString(const VMString& other) : VMString(other._size + 1) {
        assign(other.c_str());
    }

    VMString& operator=(const VMString& other) {
        assign(other.c_str());
        return *this;
    }

    VMString& operator=(const char* s) {
        assign(s);
        return *this;
    }

    ~VMString() {
        if (_buf) {
            VMManager::instance().swap_out(_page_idx);
        }
    }

    // --- Main methods ---

    // Assign a new string value
    void assign(const char* s) {
        size_t len = strlen(s);
        ensure_capacity(len + 1);
        strcpy(_buf, s);
        _size = len;
    }

    // Clear the string and free RAM
    void clear() {
        if (_buf) {
            _buf[0] = '\0';
            _size = 0;
            VMManager::instance().swap_out(_page_idx);
        }
    }

    // Append a string
    void append(const char* s) {
        size_t add_len = strlen(s);
        ensure_capacity(_size + add_len + 1);
        strcpy(_buf + _size, s);
        _size += add_len;
    }

    VMString& operator+=(const char* s) { append(s); return *this; }

    VMString& operator+=(char c) {
        ensure_capacity(_size + 2);
        _buf[_size] = c;
        _size++;
        _buf[_size] = '\0';
        return *this;
    }

    char& operator[](size_t idx) {
        if (idx >= _size) throw std::out_of_range("VMString::operator[]");
        return _buf[idx];
    }

    const char& operator[](size_t idx) const {
        if (idx >= _size) throw std::out_of_range("VMString::operator[] const");
        return _buf[idx];
    }

    const char* c_str() const { return _buf; }
    size_t size() const { return _size; }
    size_t capacity() const { return _capacity; }
    bool empty() const { return _size == 0; }

    bool operator==(const VMString& other) const {
        return strcmp(_buf, other._buf) == 0;
    }
    bool operator!=(const VMString& other) const { return !(*this == other); }

private:
    int _page_idx;
    char* _buf;
    size_t _size;
    size_t _capacity;

    // Allocate a page for the buffer
    void allocate_page(size_t min_capacity) {
        // Free the old page if needed
        if (_buf && _page_idx >= 0) VMManager::instance().swap_out(_page_idx);
        size_t alloc_size = std::max(min_capacity, (size_t)VMManager::instance().page_size);
        VMManager::instance().alloc_page(&_page_idx, true);
        _buf = reinterpret_cast<char*>(VMManager::instance().get_ptr(_page_idx, 0));
        _capacity = alloc_size;
    }

    // Ensure the buffer has enough capacity
    void ensure_capacity(size_t min_capacity) {
        if (min_capacity > _capacity) {
            // Need to expand the buffer!
            char* old_buf = _buf;
            int old_page_idx = _page_idx;
            size_t old_size = _size;

            allocate_page(min_capacity * 2); // double buffer for growth

            // Copy old content
            if (old_buf) {
                memcpy(_buf, old_buf, old_size + 1); // +1 for '\0'
                _size = old_size;
                // Swap out old page
                VMManager::instance().swap_out(old_page_idx);
            }
        }
    }
};