#include "Recompiler.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <new>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace ShaderRecompiler {

namespace {

std::atomic<std::size_t> BackingBytes{0};
std::atomic<bool> BackingCleanupFailed{false};

std::size_t BackingLimit() {
    static const auto limit = [] {
        constexpr std::size_t Default = 1u << 30u;
        const auto* value = std::getenv("APS5_FILE_BACKED_SPIRV_MAX_BYTES");
        if (value == nullptr) return Default;
        const std::string_view text(value);
        std::size_t parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? parsed : Default;
    }();
    return limit;
}

bool ReserveBacking(std::size_t bytes) {
    if (BackingCleanupFailed.load()) return false;
    const auto limit = BackingLimit();
    auto used = BackingBytes.load(std::memory_order_relaxed);
    do {
        if (used > limit || bytes > limit - used) return false;
    } while (!BackingBytes.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    return true;
}

}

struct SharedSpirv::Storage {
    mutable std::mutex mutex;
    mutable std::vector<std::uint32_t> values;
    const std::uint32_t* mapped = nullptr;
    std::size_t count = 0;
    std::size_t backingBytes = 0;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    std::array<wchar_t, MAX_PATH> fileName{};
#else
    int file = -1;
    std::string fileName;
#endif

    ~Storage() { ClearBacking(); }

    void ClearBacking() noexcept {
#ifdef _WIN32
        if (mapped != nullptr) UnmapViewOfFile(mapped);
        if (mapping != nullptr) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        mapping = nullptr;
        if (fileName[0] != 0) {
            const auto removed = DeleteFileW(fileName.data());
            const auto error = removed ? ERROR_SUCCESS : GetLastError();
            if (error == ERROR_SUCCESS || error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) fileName[0] = 0;
            else BackingCleanupFailed.store(true);
        }
#else
        if (mapped != nullptr) munmap(const_cast<std::uint32_t*>(mapped), count * sizeof(std::uint32_t));
        if (file != -1) close(file);
        file = -1;
        if (!fileName.empty()) {
            if (unlink(fileName.c_str()) == 0 || errno == ENOENT) fileName.clear();
            else BackingCleanupFailed.store(true);
        }
#endif
        mapped = nullptr;
        count = 0;
        if (backingBytes != 0) BackingBytes.fetch_sub(backingBytes, std::memory_order_relaxed);
        backingBytes = 0;
    }

    std::span<const std::uint32_t> Read() const {
        return mapped != nullptr ? std::span(mapped, count) : std::span<const std::uint32_t>(values);
    }

    void Back(std::span<const std::uint32_t> source) {
        const auto bytes = source.size_bytes();
        std::error_code error;
        const auto directory = std::filesystem::temp_directory_path(error);
        if (error) throw std::system_error(error, "SPIR-V temporary directory");
#ifdef _WIN32
        std::array<wchar_t, MAX_PATH> name{};
        if (GetTempFileNameW(directory.c_str(), L"asp", 0, name.data()) == 0) throw std::system_error(GetLastError(), std::system_category(), "SPIR-V temporary file");
        fileName = name;
        file = CreateFileW(fileName.data(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            throw std::system_error(error, std::system_category(), "SPIR-V backing file");
        }
        fileName[0] = 0;
        const auto* cursor = reinterpret_cast<const std::byte*>(source.data());
        for (std::size_t remaining = bytes; remaining != 0;) {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 64u << 20u));
            DWORD written = 0;
            if (!WriteFile(file, cursor, chunk, &written, nullptr)) throw std::system_error(GetLastError(), std::system_category(), "SPIR-V backing write");
            if (written == 0) throw std::system_error(std::make_error_code(std::errc::io_error), "SPIR-V backing write made no progress");
            cursor += written;
            remaining -= written;
        }
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping == nullptr) throw std::system_error(GetLastError(), std::system_category(), "SPIR-V file mapping");
        mapped = static_cast<const std::uint32_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, bytes));
        if (mapped == nullptr) throw std::system_error(GetLastError(), std::system_category(), "SPIR-V mapped words");
#else
        auto name = (directory / "anyps5-spv-XXXXXX").string();
        file = mkstemp(name.data());
        if (file == -1) throw std::system_error(errno, std::generic_category(), "SPIR-V backing file");
        fileName = std::move(name);
        if (unlink(fileName.c_str()) != 0) {
            const auto error = errno;
            throw std::system_error(error, std::generic_category(), "SPIR-V backing unlink");
        }
        fileName.clear();
        const auto* cursor = reinterpret_cast<const std::byte*>(source.data());
        for (std::size_t remaining = bytes; remaining != 0;) {
            const auto written = write(file, cursor, std::min<std::size_t>(remaining, 64u << 20u));
            if (written < 0 && errno == EINTR) continue;
            if (written < 0) throw std::system_error(errno, std::generic_category(), "SPIR-V backing write");
            if (written == 0) throw std::system_error(std::make_error_code(std::errc::io_error), "SPIR-V backing write made no progress");
            cursor += written;
            remaining -= written;
        }
        const auto view = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, file, 0);
        if (view == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "SPIR-V mapped words");
        mapped = static_cast<const std::uint32_t*>(view);
#endif
        count = source.size();
    }
};

SharedSpirv::SharedSpirv(std::vector<std::uint32_t> source) {
    auto storage = std::make_shared<Storage>();
    const auto* enabled = std::getenv("APS5_FILE_BACKED_SPIRV");
    if (enabled != nullptr && std::string_view(enabled) == "1" && source.size() >= 16384u && ReserveBacking(source.size() * sizeof(std::uint32_t))) {
        storage->backingBytes = source.size() * sizeof(std::uint32_t);
        try {
            storage->Back(source);
        } catch (const std::system_error&) {
            storage->ClearBacking();
        } catch (const std::bad_alloc&) {
            storage->ClearBacking();
        }
    }
    if (storage->mapped == nullptr) storage->values = std::move(source);
    words = std::move(storage);
}

SharedSpirv& SharedSpirv::operator=(std::vector<std::uint32_t> other) {
    *this = SharedSpirv(std::move(other));
    return *this;
}

std::span<const std::uint32_t> SharedSpirv::Read() const {
    return words != nullptr ? words->Read() : std::span<const std::uint32_t>();
}

const std::vector<std::uint32_t>& SharedSpirv::Words() const {
    if (words == nullptr) {
        static const std::vector<std::uint32_t> empty;
        return empty;
    }
    if (words->mapped != nullptr) {
        std::lock_guard lock(words->mutex);
        if (words->values.empty()) words->values.assign(words->mapped, words->mapped + words->count);
    }
    return words->values;
}

std::vector<std::uint32_t>& SharedSpirv::Mutable() {
    if (words == nullptr || words.use_count() != 1 || words->mapped != nullptr) {
        const auto source = Read();
        auto storage = std::make_shared<Storage>();
        storage->values.assign(source.begin(), source.end());
        words = std::move(storage);
    }
    return words->values;
}

bool operator==(const SharedSpirv& left, const SharedSpirv& right) {
    return left.words == right.words || std::ranges::equal(left.Read(), right.Read());
}

}
