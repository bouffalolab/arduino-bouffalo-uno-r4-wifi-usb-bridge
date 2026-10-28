#pragma once
// Software/fake AT framing tests only: POSIX files, no littlefs or Flash hardware.
#include "Arduino.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

class File : public Print {
    struct Handle {
        FILE *file;
        explicit Handle(FILE *f) : file(f) {}
        ~Handle() { if (file) fclose(file); }
    };
    std::shared_ptr<Handle> handle;
public:
    File() = default;
    explicit File(FILE *f) { if (f) handle = std::make_shared<Handle>(f); }
    operator bool() const { return handle && handle->file; }
    using Print::write;
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *p, size_t n) override {
        if (!*this) return 0;
        const char *limit = getenv("AT_HOST_FS_LIMIT");
        if (limit) {
            long left = atol(limit) - ftell(handle->file);
            n = left > 0 ? min(n, static_cast<size_t>(left)) : 0;
        }
        return fwrite(p, 1, n, handle->file);
    }
    int read(uint8_t *p, size_t n) {
        return *this ? static_cast<int>(fread(p, 1, n, handle->file)) : -1;
    }
    bool seek(uint32_t n) { return *this && fseek(handle->file, n, SEEK_SET) == 0; }
    size_t size() {
        if (!*this) return 0;
        long p = ftell(handle->file);
        fseek(handle->file, 0, SEEK_END);
        long n = ftell(handle->file);
        fseek(handle->file, p, SEEK_SET);
        return n;
    }
    bool close() {
        if (!*this) return false;
        int result = fclose(handle->file);
        handle->file = nullptr;
        return result == 0 && !getenv("AT_HOST_FS_CLOSE_FAIL");
    }
};

class LittleFSClass {
    bool ready = false;
    std::string path(const char *p) {
        const char *root = getenv("AT_HOST_FS_ROOT");
        if (!ready || !root || !p || p[0] != '/' || !p[1] || strstr(p, "..")) return {};
        return std::string(root) + p;
    }
public:
    bool begin() { ready = !getenv("AT_HOST_FS_UNMOUNTABLE"); return ready; }
    bool mounted() const { return ready; }
    File open(const char *p, const char *mode = "r") {
        std::string name = path(p);
        return name.empty() ? File() : File(fopen(name.c_str(), mode));
    }
    bool remove(const char *p) {
        std::string name = path(p);
        return !name.empty() && std::remove(name.c_str()) == 0;
    }
};
inline LittleFSClass LittleFS;
