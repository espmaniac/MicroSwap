#pragma once

// Minimal in-memory stand-in for the Arduino FS API, so containers.h
// can be built and tested on a desktop machine.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#define FILE_READ  "r"
#define FILE_WRITE "w"

namespace fs {

class File {
public:
    File() = default;
    explicit File(std::string* data) : _data(data) {}

    explicit operator bool() const { return _data != nullptr; }

    bool seek(size_t pos) { _pos = pos; return true; }

    size_t write(const uint8_t* buf, size_t len) {
        if (_data->size() < _pos + len) _data->resize(_pos + len);
        memcpy(&(*_data)[_pos], buf, len);
        _pos += len;
        return len;
    }

    size_t read(uint8_t* buf, size_t len) {
        size_t available = _pos < _data->size() ? _data->size() - _pos : 0;
        size_t n = len < available ? len : available;
        if (n) memcpy(buf, &(*_data)[_pos], n);
        _pos += n;
        return n;
    }

    void flush() {}
    void close() { _data = nullptr; }

private:
    std::string* _data = nullptr;
    size_t _pos = 0;
};

class FS {
public:
    bool remove(const char* path) { return _files.erase(path) > 0; }
    File open(const char* path, const char* /*mode*/) { return File(&_files[path]); }

private:
    std::map<std::string, std::string> _files;
};

} // namespace fs
