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
    typedef ptrdiff_t difference_type;

    // Random-access iterator implementation for VMVector
    class iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using reference = T&;
        using pointer = T*;
        using difference_type = ptrdiff_t;

        iterator(VMVector* vec, size_type pos) : _vec(vec), _pos(pos) {}

        reference operator*() { return (*_vec)[_pos]; }
        pointer operator->() { return &(*_vec)[_pos]; }
        iterator& operator++() { ++_pos; return *this; }
        iterator operator++(int) { iterator tmp = *this; ++_pos; return tmp; }
        iterator& operator--() { --_pos; return *this; }
        iterator operator--(int) { iterator tmp = *this; --_pos; return tmp; }
        iterator& operator+=(difference_type n) { _pos += n; return *this; }
        iterator& operator-=(difference_type n) { _pos -= n; return *this; }
        iterator operator+(difference_type n) const { return iterator(_vec, _pos + n); }
        iterator operator-(difference_type n) const { return iterator(_vec, _pos - n); }
        difference_type operator-(const iterator& rhs) const { return _pos - rhs._pos; }
        reference operator[](difference_type n) { return (*_vec)[_pos + n]; }
        bool operator==(const iterator& other) const { return _vec == other._vec && _pos == other._pos; }
        bool operator!=(const iterator& other) const { return !(*this == other); }
        bool operator<(const iterator& other) const { return _pos < other._pos; }
        bool operator>(const iterator& other) const { return _pos > other._pos; }
        bool operator<=(const iterator& other) const { return _pos <= other._pos; }
        bool operator>=(const iterator& other) const { return _pos >= other._pos; }

    private:
        VMVector* _vec;
        size_type _pos;
        friend class VMVector;
    };

    class const_iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using reference = const T&;
        using pointer = const T*;
        using difference_type = ptrdiff_t;

        const_iterator(const VMVector* vec, size_type pos) : _vec(vec), _pos(pos) {}

        reference operator*() const { return (*_vec)[_pos]; }
        pointer operator->() const { return &(*_vec)[_pos]; }
        const_iterator& operator++() { ++_pos; return *this; }
        const_iterator operator++(int) { const_iterator tmp = *this; ++_pos; return tmp; }
        const_iterator& operator--() { --_pos; return *this; }
        const_iterator operator--(int) { const_iterator tmp = *this; --_pos; return tmp; }
        const_iterator& operator+=(difference_type n) { _pos += n; return *this; }
        const_iterator& operator-=(difference_type n) { _pos -= n; return *this; }
        const_iterator operator+(difference_type n) const { return const_iterator(_vec, _pos + n); }
        const_iterator operator-(difference_type n) const { return const_iterator(_vec, _pos - n); }
        difference_type operator-(const const_iterator& rhs) const { return _pos - rhs._pos; }
        reference operator[](difference_type n) const { return (*_vec)[_pos + n]; }
        bool operator==(const const_iterator& other) const { return _vec == other._vec && _pos == other._pos; }
        bool operator!=(const const_iterator& other) const { return !(*this == other); }
        bool operator<(const const_iterator& other) const { return _pos < other._pos; }
        bool operator>(const const_iterator& other) const { return _pos > other._pos; }
        bool operator<=(const const_iterator& other) const { return _pos <= other._pos; }
        bool operator>=(const const_iterator& other) const { return _pos >= other._pos; }

    private:
        const VMVector* _vec;
        size_type _pos;
        friend class VMVector;
    };

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

    // Copy constructor
    VMVector(const VMVector& other) : VMVector() {
        assign(other.begin(), other.end());
    }

    // Move constructor
    VMVector(VMVector&& other) noexcept : _chunk_capacity(other._chunk_capacity), _size(other._size), _chunk_count(other._chunk_count) {
        for (size_type i = 0; i < VM_PAGE_COUNT; ++i) {
            _chunks[i] = other._chunks[i];
            other._chunks[i].page_idx = -1;
            other._chunks[i].count = 0;
        }
        other._size = 0;
        other._chunk_count = 0;
    }

    // Copy assignment
    VMVector& operator=(const VMVector& other) {
        if (this != &other) {
            clear();
            assign(other.begin(), other.end());
        }
        return *this;
    }

    // Move assignment
    VMVector& operator=(VMVector&& other) noexcept {
        if (this != &other) {
            clear();
            _chunk_capacity = other._chunk_capacity;
            _size = other._size;
            _chunk_count = other._chunk_count;
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

    ~VMVector() { clear(); }

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

    reference front() {
        if (_size == 0) throw std::out_of_range("front from empty VMVector");
        return (*this)[0];
    }

    const_reference front() const {
        if (_size == 0) throw std::out_of_range("front from empty VMVector");
        return (*this)[0];
    }

    reference back() {
        if (_size == 0) throw std::out_of_range("back from empty VMVector");
        return (*this)[_size - 1];
    }

    const_reference back() const {
        if (_size == 0) throw std::out_of_range("back from empty VMVector");
        return (*this)[_size - 1];
    }

    bool empty() const { return _size == 0; }
    size_type size() const { return _size; }
    size_type capacity() const { return _chunk_count * _chunk_capacity; }

    // Reserve memory for at least n elements (allocate enough pages)
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

    // Reduces capacity to fit size (frees unused pages)
    void shrink_to_fit() {
        size_type used_chunks = (_size + _chunk_capacity - 1) / _chunk_capacity;
        for (size_type i = used_chunks; i < _chunk_count; ++i) {
            if (_chunks[i].page_idx != -1) {
                VMManager::instance().swap_out(_chunks[i].page_idx);
                _chunks[i].page_idx = -1;
                _chunks[i].count = 0;
            }
        }
        _chunk_count = used_chunks;
    }

    // Swap contents with another VMVector
    void swap(VMVector& other) {
        std::swap(_chunk_capacity, other._chunk_capacity);
        std::swap(_size, other._size);
        std::swap(_chunk_count, other._chunk_count);
        for (size_type i = 0; i < VM_PAGE_COUNT; ++i) {
            std::swap(_chunks[i], other._chunks[i]);
        }
    }

    iterator insert(iterator pos, const T& value) {
        size_type idx = pos - begin();
        push_back(T());
        for (size_type i = _size - 1; i > idx; --i) {
            (*this)[i] = (*this)[i - 1];
        }
        (*this)[idx] = value;
        return iterator(this, idx);
    }

    iterator erase(iterator pos) {
        size_type idx = pos - begin();
        if (idx >= _size) return end();
        for (size_type i = idx; i < _size - 1; ++i) {
            (*this)[i] = (*this)[i + 1];
        }
        pop_back();
        return iterator(this, idx);
    }

    void clear() {
        for (size_type i = 0; i < _chunk_count; ++i) {
            Chunk& ch = _chunks[i];
            for (size_type j = 0; j < ch.count; ++j) {
                T* ptr = reinterpret_cast<T*>(VMManager::instance().get_ptr(ch.page_idx, j * sizeof(T)));
                ptr->~T();
            }
            VMManager::instance().swap_out(ch.page_idx); // free RAM!
            ch.page_idx = -1;
            ch.count = 0;
        }
        _chunk_count = 0;
        _size = 0;
    }

    void resize(size_type n, const T& val = T()) {
        if (n < _size) {
            while (_size > n) pop_back();
        } else if (n > _size) {
            while (_size < n) push_back(val);
        }
    }

    void assign(size_type n, const T& val) {
        clear();
        resize(n, val);
    }
    template<typename InputIt>
    void assign(InputIt first, InputIt last) {
        clear();
        for (InputIt it = first; it != last; ++it) push_back(*it);
    }

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, _size); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, _size); }
    const_iterator cbegin() const { return const_iterator(this, 0); }
    const_iterator cend() const { return const_iterator(this, _size); }

    bool operator==(const VMVector& other) const {
        if (_size != other._size) return false;
        for (size_type i = 0; i < _size; ++i) {
            if ((*this)[i] != other[i]) return false;
        }
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
    struct Chunk {
        int page_idx;
        size_type count;
    };
    Chunk _chunks[VM_PAGE_COUNT];
    size_type _chunk_count;
    size_type _chunk_capacity;
    size_type _size;

    friend class iterator;
    friend class const_iterator;
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

    void fill(const T& val) {
        for (size_type i = 0; i < N; ++i) (*this)[i] = val;
    }

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

    void assign(const char* s) {
        size_t len = strlen(s);
        ensure_capacity(len + 1);
        strcpy(_buf, s);
        _size = len;
    }

    void clear() {
        if (_buf) {
            _buf[0] = '\0';
            _size = 0;
            VMManager::instance().swap_out(_page_idx);
        }
    }

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

    void allocate_page(size_t min_capacity) {
        if (_buf && _page_idx >= 0) VMManager::instance().swap_out(_page_idx);
        size_t alloc_size = std::max(min_capacity, (size_t)VMManager::instance().page_size);
        VMManager::instance().alloc_page(&_page_idx, true);
        _buf = reinterpret_cast<char*>(VMManager::instance().get_ptr(_page_idx, 0));
        _capacity = alloc_size;
    }

    void ensure_capacity(size_t min_capacity) {
        if (min_capacity > _capacity) {
            char* old_buf = _buf;
            int old_page_idx = _page_idx;
            size_t old_size = _size;

            allocate_page(min_capacity * 2);

            if (old_buf) {
                memcpy(_buf, old_buf, old_size + 1);
                _size = old_size;
                VMManager::instance().swap_out(old_page_idx);
            }
        }
    }
};
