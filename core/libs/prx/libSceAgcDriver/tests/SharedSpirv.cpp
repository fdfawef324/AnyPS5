#include "Recompiler.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace {

void Require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}

void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    Require(_putenv_s(name, value != nullptr ? value : "") == 0, "cannot set test environment");
#else
    Require((value != nullptr ? setenv(name, value, 1) : unsetenv(name)) == 0, "cannot set test environment");
#endif
}

class TemporaryEnvironment {
public:
    explicit TemporaryEnvironment(const std::filesystem::path& path) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (const auto* value = std::getenv(names[i])) original[i] = value;
            SetEnvironment(names[i], path.string().c_str());
        }
    }
    ~TemporaryEnvironment() {
        for (std::size_t i = 0; i < names.size(); ++i) SetEnvironment(names[i], original[i] ? original[i]->c_str() : nullptr);
    }
private:
    static constexpr std::array<const char*, 3> names{"TMP", "TEMP", "TMPDIR"};
    std::array<std::optional<std::string>, 3> original;
};

ShaderRecompiler::SharedSpirv Module(bool mapped) {
    std::vector<std::uint32_t> values(16384u, 0x12345678u);
    const auto* pointer = values.data();
    ShaderRecompiler::SharedSpirv module(std::move(values));
    Require((module.Read().data() != pointer) == mapped, "module does not obey backing budget or fallback");
    Require(module.Read().size() == 16384u && module.Read().front() == 0x12345678u && module.Read().back() == 0x12345678u, "backing fallback changed module words");
    return module;
}

void Budget() {
    using ShaderRecompiler::SharedSpirv;
    auto first = Module(true);
    auto copy = first;
    auto moved = std::move(first);
    auto second = Module(false);
    moved = SharedSpirv{};
    auto retained = Module(false);
    copy = SharedSpirv{};
    auto released = Module(true);
    released[0] = 0xabcdef01u;
    auto detached = Module(true);
    Require(second.Read()[0] == 0x12345678u && retained.Read()[0] == 0x12345678u && released.Read()[0] == 0xabcdef01u, "budget transitions changed surviving module owners");
    detached = SharedSpirv{};
    std::array<SharedSpirv, 4> concurrent;
    std::array<std::thread, 4> workers;
    std::atomic<unsigned> mapped{0};
    std::atomic<bool> correct{true};
    for (std::size_t i = 0; i < workers.size(); ++i) workers[i] = std::thread([&, i] {
        std::vector<std::uint32_t> values(16384u, static_cast<std::uint32_t>(i));
        const auto* pointer = values.data();
        concurrent[i] = SharedSpirv(std::move(values));
        if (concurrent[i].Read().data() != pointer) mapped.fetch_add(1);
        if (concurrent[i].Read().size() != 16384u || concurrent[i].Read()[9000] != i) correct.store(false);
    });
    for (auto& worker : workers) worker.join();
    Require(correct.load() && mapped.load() == 1u, "concurrent backing reservations exceeded or lost the budget");
    concurrent = {};
    auto reused = Module(true);
}

void IoFailure() {
    const auto name = "anyps5-spv-unusable-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto file = std::filesystem::temp_directory_path() / name;
    {
        std::ofstream stream(file);
        Require(stream.good(), "cannot create the unusable temporary directory fixture");
    }
    try {
        {
            TemporaryEnvironment environment(file);
            auto fallback = Module(false);
            Require(std::filesystem::is_regular_file(file) && std::filesystem::file_size(file) == 0u, "failed temporary backing removed the existing blocked path");
            auto alias = fallback;
            fallback[0] = 0xabcdef01u;
            Require(alias.Read()[0] == 0x12345678u && fallback.Read()[0] == 0xabcdef01u, "I/O fallback lost copy-on-write isolation");
        }
        auto next = Module(true);
        std::filesystem::remove(file);
    } catch (...) {
        std::filesystem::remove(file);
        throw;
    }
}

#ifdef _WIN32
std::size_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX info{};
    info.cb = sizeof(info);
    Require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info)), "process memory counters unavailable");
    return info.PrivateUsage;
}
#endif

}

int main(int argc, char** argv) {
    try {
        using ShaderRecompiler::SharedSpirv;
        if (argc == 2 && std::string_view(argv[1]) == "--backing-budget") {
            Budget();
            std::puts("SharedSpirv budget: sharing, moves, detaches, releases and concurrent reservations passed");
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--backing-io-failure") {
            IoFailure();
            std::puts("SharedSpirv real temporary-file failure preserves words and releases its backing reservation");
            return 0;
        }
        Require(argc == 1, "unknown shared SPIR-V test arguments");
        constexpr std::size_t Count = 16u << 20u;
        constexpr std::uint32_t Pattern = 0x12345678u;
        const bool backed = std::getenv("APS5_FILE_BACKED_SPIRV") != nullptr && std::string_view(std::getenv("APS5_FILE_BACKED_SPIRV")) == "1";
#ifdef _WIN32
        const auto before = PrivateBytes();
#endif
        SharedSpirv original(std::vector<std::uint32_t>(Count, Pattern));
        const auto& read = original;
        const auto* pointer = read.data();
        Require(read.size() == Count && !read.empty(), "module extent changed");
#ifdef _WIN32
        const auto after = PrivateBytes();
        std::printf("Private bytes delta: %lld\n", static_cast<long long>(after) - static_cast<long long>(before));
        MEMORY_BASIC_INFORMATION region{};
        Require(VirtualQuery(pointer, &region, sizeof(region)) == sizeof(region), "module memory query failed");
        Require((region.Type == MEM_MAPPED) == backed, "module uses the wrong backing type");
        if (backed) {
            Require(region.Protect == PAGE_READONLY, "file-backed module permits writes");
            Require(after <= before + (16u << 20u), "file-backed module retains private commit");
        }
#endif
        std::vector<SharedSpirv> copies(8, original);
        for (const auto& copy : copies) Require(copy.data() == pointer && copy[50000] == Pattern, "shared immutable words changed");
        copies.front()[50000] = 0xabcdef01u;
        Require(read[50000] == Pattern && static_cast<const SharedSpirv&>(copies.front())[50000] == 0xabcdef01u, "indexed write did not detach");
        auto& edited = copies[1];
        edited.data()[Count - 1] = 0xffu;
        edited.insert(edited.begin() + 4, {0xfeedu});
        edited.resize(Count + 2);
        Require(edited.size() == Count + 2 && read.size() == Count && read[Count - 1] == Pattern, "vector editing changed another holder");
        Require(read[4] == Pattern && static_cast<const SharedSpirv&>(edited)[4] == 0xfeedu, "insertion lost its position");
        const auto& legacy = copies[2].Words();
        Require(legacy.size() == Count && legacy[50000] == Pattern, "legacy vector read changed words");
        original = SharedSpirv{};
        Require(static_cast<const SharedSpirv&>(copies.back())[Count - 1] == Pattern, "words did not survive the first owner");
        std::array<std::thread, 4> readers;
        std::atomic<bool> correct{true};
        for (auto& reader : readers) reader = std::thread([&] {
            const auto& copy = copies.back();
            if (copy.Words()[50000] != Pattern || copy.Read()[Count - 1] != Pattern) correct.store(false);
        });
        for (auto& reader : readers) reader.join();
        Require(correct.load(), "concurrent reads changed words");
        SharedSpirv empty;
        Require(empty.empty() && empty.Read().empty() && empty == SharedSpirv{}, "empty module changed");
        std::printf("SharedSpirv backing=%u: sharing, write isolation, lifetime and reads passed\n", backed);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
