#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Monitor::Virtual {
    constexpr size_t MAX_REGIONS = 3;

    // A horizontal slice of a host output, in host pixels.
    struct SRegionSpan {
        int64_t x     = 0;
        int64_t width = 0;
    };

    // The smallest pixel count that is a whole number of logical pixels at this scale,
    // e.g. 5 at 1.25, 3 at 1.5, 2 at 2. 1 if there is none up to 120.
    int64_t logicalPixelStep(double scale);

    // Splits pixelWidth into ratios.size() adjacent spans that cover it exactly, with
    // every boundary on a logicalPixelStep() multiple. The last span takes the rest.
    std::expected<std::vector<SRegionSpan>, std::string> partition(int64_t pixelWidth, double scale, std::span<const double> ratios, double minLogicalWidth);

    // Ratios for a named preset: "equal" (any count) or "center" (3 regions, 25/50/25).
    std::expected<std::vector<double>, std::string> presetRatios(std::string_view preset, size_t count);
}
