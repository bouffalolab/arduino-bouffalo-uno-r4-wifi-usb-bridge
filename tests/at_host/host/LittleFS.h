#pragma once
// Software/fake AT framing tests only: POSIX files, no littlefs or Flash hardware.
#include "Arduino.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <cerrno>
#include <sys/stat.h>

enum { LFS_ERR_IO=-5, LFS_ERR_NOENT=-2, LFS_ERR_BADF=-9, LFS_ERR_INVAL=-22,
       LFS_ERR_NOSPC=-28, LFS_ERR_NOMEM=-12, LFS_ERR_CORRUPT=-84 };

class File : public Print {
    struct Handle {
        FILE *file;
        int error = 0;
        explicit Handle(FILE *f) : file(f) {}
        ~Handle() { if (file) fclose(file); }
    };
    std::shared_ptr<Handle> handle;
public:
    File() = default;
    explicit File(FILE *f) { if (f) handle = std::make_shared<Handle>(f); }
    operator bool() const { return handle && handle->file; }
    using Print::write;
    int lastError() const { return handle ? handle->error : LFS_ERR_BADF; }
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *p, size_t n) override {
        if (!*this) return 0;
        const char *limit = getenv("AT_HOST_FS_LIMIT");
        size_t requested = n;
        if (limit) {
            long left = atol(limit) - ftell(handle->file);
            n = left > 0 ? min(n, static_cast<size_t>(left)) : 0;
        }
        size_t written = fwrite(p, 1, n, handle->file);
        handle->error = written == requested ? 0 : LFS_ERR_NOSPC;
        return written;
    }
    int read(uint8_t *p, size_t n) {
        return *this ? static_cast<int>(fread(p, 1, n, handle->file)) : -1;
    }
    bool seek(uint32_t n) { return *this && fseek(handle->file, n, SEEK_SET) == 0; }
    size_t position() { return *this ? ftell(handle->file) : 0; }
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
        handle->error = result == 0 && !getenv("AT_HOST_FS_CLOSE_FAIL") ? 0 : LFS_ERR_IO;
        return handle->error == 0;
    }
};

class LittleFSClass {
    bool ready = false;
    int error = 0;
    std::string path(const char *p) {
        const char *root = getenv("AT_HOST_FS_ROOT");
        if (!ready || !root || !p || p[0] != '/' || !p[1] || strstr(p, "..")) return {};
        return std::string(root) + p;
    }
public:
    bool begin() { ready = !getenv("AT_HOST_FS_UNMOUNTABLE"); error = ready ? 0 : LFS_ERR_IO; return ready; }
    bool mounted() const { return ready; }
    int lastError() const { return error; }
    File open(const char *p, const char *mode = "r") {
        std::string name = path(p);
        if (name.empty()) { error = LFS_ERR_INVAL; return File(); }
        FILE *f = fopen(name.c_str(), mode); error = f ? 0 : -errno;
        return File(f);
    }
    bool remove(const char *p) {
        std::string name = path(p);
        bool ok = !name.empty() && std::remove(name.c_str()) == 0;
        error = ok ? 0 : -errno; return ok;
    }
    bool exists(const char *p) { struct stat s; std::string n=path(p); bool ok=!n.empty() && stat(n.c_str(),&s)==0; error=ok?0:-errno; return ok; }
    bool mkdir(const char *p) { std::string n=path(p); bool ok=!n.empty() && ::mkdir(n.c_str(),0700)==0; error=ok?0:-errno; return ok; }
    bool rename(const char *a,const char *b) {
        std::string from=path(a),to=path(b);
        bool ok=!getenv("AT_HOST_FS_RENAME_FAIL") && !from.empty() && !to.empty() && std::rename(from.c_str(),to.c_str())==0;
        error=ok?0:LFS_ERR_IO; return ok;
    }
};
inline LittleFSClass LittleFS;
