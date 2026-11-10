#include "VirtualRegions.hpp"

#include <hyprutils/memory/Casts.hpp>

#include <cmath>
#include <format>

using namespace Monitor::Virtual;
using namespace Hyprutils::Memory;

int64_t Monitor::Virtual::logicalPixelStep(double scale) {
    if (!std::isfinite(scale) || scale <= 0)
        return 1;

    for (int64_t step = 1; step <= 120; ++step) {
        const double LOGICAL = sc<double>(step) / scale;
        if (std::abs(LOGICAL - std::round(LOGICAL)) < 1e-6)
            return step;
    }

    return 1;
}

std::expected<std::vector<SRegionSpan>, std::string> Monitor::Virtual::partition(int64_t pixelWidth, double scale, std::span<const double> ratios, double minLogicalWidth) {
    if (ratios.empty() || ratios.size() > MAX_REGIONS)
        return std::unexpected(std::format("need 1 to {} regions, got {}", MAX_REGIONS, ratios.size()));

    if (pixelWidth <= 0)
        return std::unexpected("the output has no width");

    if (!std::isfinite(scale) || scale <= 0)
        return std::unexpected("invalid scale");

    double total = 0;
    for (const auto r : ratios) {
        if (!std::isfinite(r) || r <= 0)
            return std::unexpected("region proportions must be positive");
        total += r;
    }

    const int64_t            STEP = logicalPixelStep(scale);
    std::vector<SRegionSpan> spans;
    spans.reserve(ratios.size());

    double  cumulative = 0;
    int64_t start      = 0;
    for (size_t i = 0; i < ratios.size(); ++i) {
        cumulative += ratios[i];

        int64_t end = pixelWidth;
        if (i + 1 < ratios.size())
            end = std::llround(cumulative / total * sc<double>(pixelWidth) / sc<double>(STEP)) * STEP;

        if (end <= start || end > pixelWidth)
            return std::unexpected(std::format("region {} would be empty", i + 1));

        spans.push_back({.x = start, .width = end - start});
        start = end;
    }

    for (size_t i = 0; i < spans.size(); ++i) {
        const double LOGICAL = sc<double>(spans[i].width) / scale;
        if (LOGICAL + 1e-6 < minLogicalWidth)
            return std::unexpected(std::format("region {} would be {:.0f} logical pixels wide, less than the minimum of {:.0f}", i + 1, LOGICAL, minLogicalWidth));
    }

    return spans;
}

std::expected<std::vector<double>, std::string> Monitor::Virtual::presetRatios(std::string_view preset, size_t count) {
    if (count == 0 || count > MAX_REGIONS)
        return std::unexpected(std::format("need 1 to {} regions, got {}", MAX_REGIONS, count));

    if (preset == "equal" || preset.empty())
        return std::vector<double>(count, 1.0);

    if (preset == "center" || preset == "center-focused") {
        if (count != 3)
            return std::unexpected("the center-focused preset needs 3 regions");
        return std::vector<double>{1.0, 2.0, 1.0};
    }

    return std::unexpected(std::format("unknown preset \"{}\"", preset));
}
