// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace anygear::d4xx {

struct IrBgrOptions {
    bool auto_contrast = true;
    unsigned int low_percentile = 1;
    unsigned int high_percentile = 99;
    int minimum_range = 24;
};

struct IrBgrStats {
    std::uint8_t low = 0;
    std::uint8_t high = 255;
};

// Convert one librealsense Y8 image to the BGR24 layout consumed by the
// original VisionPose wrapper. The source stride may include row padding; the
// output is always tightly packed.
bool ir_y8_to_bgr24(const std::uint8_t* source,
                    std::size_t source_bytes,
                    int width,
                    int height,
                    int source_stride,
                    const IrBgrOptions& options,
                    std::vector<std::uint8_t>* output,
                    IrBgrStats* stats = nullptr);

} // namespace anygear::d4xx
