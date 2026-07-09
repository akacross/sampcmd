// Low-level Win32 conveniences: RAII wrappers and error-string formatting.
//
// These are deliberately dependency-free (no Logger) so every other module can
// use them without creating include cycles.
#pragma once

#include "windows_prelude.hpp"
#include <string>
#include <utility>

namespace win32 {

// Owns a HANDLE and closes it exactly once. Treats both nullptr and
// INVALID_HANDLE_VALUE as "empty" because the Win32 API is inconsistent about
// which one signals failure.
class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) noexcept : h_(h) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& o) noexcept : h_(o.release()) {}
    ScopedHandle& operator=(ScopedHandle&& o) noexcept {
        if (this != &o) { reset(o.release()); }
        return *this;
    }
    ~ScopedHandle() { reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] bool valid() const noexcept { return h_ && h_ != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return valid(); }

    HANDLE release() noexcept { return std::exchange(h_, nullptr); }
    void reset(HANDLE h = nullptr) noexcept {
        if (valid()) ::CloseHandle(h_);
        h_ = h;
    }

private:
    HANDLE h_ = nullptr;
};

// Owns a block allocated with VirtualAllocEx in a *remote* process and frees it
// with VirtualFreeEx. The handle is not owned; it must outlive this object.
class RemoteMemory {
public:
    RemoteMemory() = default;
    RemoteMemory(HANDLE process, void* addr) noexcept : process_(process), addr_(addr) {}
    RemoteMemory(const RemoteMemory&) = delete;
    RemoteMemory& operator=(const RemoteMemory&) = delete;
    RemoteMemory(RemoteMemory&& o) noexcept
        : process_(o.process_), addr_(std::exchange(o.addr_, nullptr)) {}
    RemoteMemory& operator=(RemoteMemory&& o) noexcept {
        if (this != &o) {
            free();
            process_ = o.process_;
            addr_ = std::exchange(o.addr_, nullptr);
        }
        return *this;
    }
    ~RemoteMemory() { free(); }

    [[nodiscard]] void* get() const noexcept { return addr_; }
    [[nodiscard]] bool valid() const noexcept { return addr_ != nullptr; }
    void* release() noexcept { return std::exchange(addr_, nullptr); }

private:
    void free() noexcept {
        if (addr_ && process_) ::VirtualFreeEx(process_, addr_, 0, MEM_RELEASE);
        addr_ = nullptr;
    }
    HANDLE process_ = nullptr;
    void*  addr_    = nullptr;
};

// FormatMessageW wrapper: "(0x00000002) The system cannot find the file specified."
std::wstring format_error(DWORD error);

// Convenience: format_error(GetLastError()).
std::wstring last_error();

} // namespace win32
