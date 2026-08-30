// SPDX-License-Identifier: GPL-3.0-or-later
#include "d4xx/ir_bgr.h"

#include <algorithm>
#include <array>
#include <limits>

namespace anygear::d4xx {

namespace {

std::uint8_t percentile_value(
    const std::array<std::size_t, 256>& histogram,
    std::size_t pixels,
    unsigned int percentile) {
    const std::size_t target = std::min(
        pixels - 1,
        pixels * static_cast<std::size_t>(percentile) / 100);
    std::size_t cumulative = 0;
    for (std::size_t value = 0; value < histogram.size(); ++value) {
        cumulative += histogram[value];
        if (cumulative > target) {
            return static_cast<std::uint8_t>(value);
        }
    }
    return 255;
}

} // namespace

bool ir_y8_to_bgr24(const std::uint8_t* source,
                    std::size_t source_bytes,
                    int width,
                    int height,
                    int source_stride,
                    const IrBgrOptions& options,
                    std::vector<std::uint8_t>* output,
                    IrBgrStats* stats) {
    if (!source || !output || width <= 0 || height <= 0 ||
        source_stride < width || options.low_percentile > 100 ||
        options.high_percentile > 100 ||
        options.low_percentile >= options.high_percentile) {
        return false;
    }
    const std::size_t rows = static_cast<std::size_t>(height);
    const std::size_t stride = static_cast<std::size_t>(source_stride);
    if (rows > std::numeric_limits<std::size_t>::max() / stride ||
        source_bytes < rows * stride) {
        return false;
    }

    std::uint8_t low = 0;
    std::uint8_t high = 255;
    if (options.auto_contrast) {
        std::array<std::size_t, 256> histogram{};
        for (int y = 0; y < height; ++y) {
            const auto* row = source + static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < width; ++x) {
                ++histogram[row[x]];
            }
        }
        const std::size_t pixels =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        low = percentile_value(histogram, pixels, options.low_percentile);
        high = percentile_value(histogram, pixels, options.high_percentile);
        if (static_cast<int>(high) - static_cast<int>(low) <
            std::max(1, options.minimum_range)) {
            low = 0;
            high = 255;
        }
    }

    const int range = std::max(
        1, static_cast<int>(high) - static_cast<int>(low));
    const std::size_t pixels =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    output->resize(pixels * 3);
    for (int y = 0; y < height; ++y) {
        const auto* row = source + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const int stretched = std::clamp(
                (static_cast<int>(row[x]) - static_cast<int>(low)) * 255 /
                    range,
                0, 255);
            const auto value = static_cast<std::uint8_t>(stretched);
            const std::size_t destination =
                (static_cast<std::size_t>(y) * width + x) * 3;
            (*output)[destination + 0] = value;
            (*output)[destination + 1] = value;
            (*output)[destination + 2] = value;
        }
    }
    if (stats) {
        stats->low = low;
        stats->high = high;
    }
    return true;
}

} // namespace anygear::d4xx
