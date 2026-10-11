#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPATCHCHUNKS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DISPATCHCHUNKS_HPP

#include <algorithm>
#include <array>
#include <cstdint>

namespace AgcDriver {

using DispatchGroups = std::array<std::uint32_t, 3>;

inline bool DispatchWithinLimits(const DispatchGroups& groups, const std::uint32_t* limit) {
    return groups[0] <= limit[0] && groups[1] <= limit[1] && groups[2] <= limit[2];
}

template<typename TVisit>
void ForEachDispatchChunk(const DispatchGroups& groups, const std::uint32_t* limit, TVisit&& visit) {
    for (std::uint32_t z = 0; z < groups[2]; z += std::min(groups[2] - z, limit[2])) {
        for (std::uint32_t y = 0; y < groups[1]; y += std::min(groups[1] - y, limit[1])) {
            for (std::uint32_t x = 0; x < groups[0]; x += std::min(groups[0] - x, limit[0])) {
                visit(DispatchGroups{x, y, z}, DispatchGroups{std::min(groups[0] - x, limit[0]), std::min(groups[1] - y, limit[1]), std::min(groups[2] - z, limit[2])});
            }
        }
    }
}

}

#endif
