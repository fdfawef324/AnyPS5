#include "prx/libSceAgcDriver/Execution/include/DispatchChunks.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::DispatchGroups;
using AgcDriver::Graphics::Require;

constexpr std::uint32_t Rows = 65537;
constexpr std::uint32_t Untouched = 0xcafebabeu;
alignas(256) constexpr std::array<std::uint32_t, 5> Code{0x7e000204u, 0x34020082u, 0xe0701000u, 0x80000001u, 0xbf810000u};
alignas(256) std::array<std::uint32_t, Rows + 1> Output{};

std::vector<std::pair<DispatchGroups, DispatchGroups>> Chunks(const DispatchGroups& groups, const DispatchGroups& limit) {
    std::vector<std::pair<DispatchGroups, DispatchGroups>> chunks;
    AgcDriver::ForEachDispatchChunk(groups, limit.data(), [&](const DispatchGroups& base, const DispatchGroups& count) { chunks.emplace_back(base, count); });
    return chunks;
}

void ChunkTests() {
    const DispatchGroups minimum{65535, 65535, 65535};
    const auto wide = Chunks({129199, 1, 1}, minimum);
    Require(wide.size() == 2 && wide[0] == std::pair{DispatchGroups{0, 0, 0}, DispatchGroups{65535, 1, 1}} && wide[1] == std::pair{DispatchGroups{65535, 0, 0}, DispatchGroups{63664, 1, 1}}, "a dispatch just under twice the X limit is not two chunks along X");
    const auto exact = Chunks({65535, 65535, 1}, minimum);
    Require(exact.size() == 1 && exact[0] == std::pair{DispatchGroups{0, 0, 0}, DispatchGroups{65535, 65535, 1}}, "a dispatch at the limits was split");
    Require(Chunks({0, 5, 5}, minimum).empty() && Chunks({5, 0, 5}, minimum).empty(), "an empty dispatch produced chunks");

    const DispatchGroups grid{7, 3, 2};
    const DispatchGroups small{3, 2, 2};
    const auto tiles = Chunks(grid, small);
    Require(tiles.size() == 6, "a 7x3x2 dispatch under 3x2x2 limits is not 6 chunks");
    std::vector<int> covered(grid[0] * grid[1] * grid[2]);
    for (const auto& [base, count] : tiles) {
        Require(count[0] != 0 && count[1] != 0 && count[2] != 0 && AgcDriver::DispatchWithinLimits(count, small.data()), "a chunk is empty or exceeds the limits");
        for (std::uint32_t z = base[2]; z < base[2] + count[2]; ++z) {
            for (std::uint32_t y = base[1]; y < base[1] + count[1]; ++y) {
                for (std::uint32_t x = base[0]; x < base[0] + count[0]; ++x) {
                    Require(x < grid[0] && y < grid[1] && z < grid[2], "a chunk reaches past the dispatch");
                    ++covered[(z * grid[1] + y) * grid[0] + x];
                }
            }
        }
    }
    for (std::size_t cell = 0; cell < covered.size(); ++cell) Require(covered[cell] == 1, "group " + std::to_string(cell) + " is covered " + std::to_string(covered[cell]) + " times");
}

std::vector<std::uint32_t> Commands(std::uint32_t rows) {
    std::vector<std::uint32_t> words;
    const auto registers = [&](std::uint32_t first, std::span<const std::uint32_t> values) {
        words.push_back(0xc0007600u | (static_cast<std::uint32_t>(values.size()) << 16u));
        words.push_back(first);
        words.insert(words.end(), values.begin(), values.end());
    };
    const std::array<std::uint32_t, 3> shape{1, 1, 1};
    registers(0x207, shape);
    const auto program = reinterpret_cast<std::uintptr_t>(Code.data());
    const std::array<std::uint32_t, 2> address{static_cast<std::uint32_t>(program >> 8u), static_cast<std::uint32_t>(program >> 40u)};
    registers(0x20c, address);
    const std::array<std::uint32_t, 1> resource{(4u << 1u) | (1u << 8u)};
    registers(0x213, resource);
    const auto output = reinterpret_cast<std::uintptr_t>(Output.data());
    const std::array<std::uint32_t, 4> descriptor{static_cast<std::uint32_t>(output), static_cast<std::uint32_t>((output >> 32u) & 0xffffu), static_cast<std::uint32_t>(sizeof(Output)), 0x31016facu};
    registers(0x240, descriptor);
    words.insert(words.end(), {0xc0031500u, 1, rows, 1, 0x41u});
    return words;
}

void ExecuteTests() {
    Output.fill(Untouched);
    const auto words = Commands(Rows);
    Packet packet{const_cast<std::uint32_t*>(words.data()), static_cast<std::uint32_t>(words.size()), 0, {}};
    AgcDriver::Submit(&packet, 0);
    AgcDriverWaitIdle_nid_postfix();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(Output.data()), sizeof(Output));
    for (std::uint32_t row = 0; row < Rows; ++row) Require(Output[row] == row, "group " + std::to_string(row) + " of a " + std::to_string(Rows) + "-row dispatch stored " + std::to_string(Output[row]));
    Require(Output[Rows] == Untouched, "the dispatch stored past its last group");
}

}

int main() {
    try {
        ChunkTests();
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        ExecuteTests();
        AgcDriverShutdown_nid_postfix();
        std::puts("dispatch split tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
