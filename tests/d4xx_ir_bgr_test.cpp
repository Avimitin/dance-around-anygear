// SPDX-License-Identifier: GPL-3.0-or-later
#include "d4xx/ir_bgr.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

bool expect(bool value, const char* message) {
    if (value) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

} // namespace

int main() {
    const std::array<std::uint8_t, 8> padded{{
        0, 64, 0xee, 0xee,
        128, 255, 0xee, 0xee,
    }};
    anygear::d4xx::IrBgrOptions options;
    options.auto_contrast = false;
    std::vector<std::uint8_t> output;
    if (!expect(anygear::d4xx::ir_y8_to_bgr24(
                    padded.data(), padded.size(), 2, 2, 4,
                    options, &output),
                "padded Y8 conversion failed")) {
        return 1;
    }
    const std::array<std::uint8_t, 12> expected{{
        0, 0, 0, 64, 64, 64,
        128, 128, 128, 255, 255, 255,
    }};
    if (!expect(output.size() == expected.size(), "BGR24 size is wrong") ||
        !expect(std::equal(output.begin(), output.end(), expected.begin()),
                "Y8 samples were not replicated into BGR order")) {
        return 1;
    }

    const std::array<std::uint8_t, 4> narrow{{100, 101, 102, 103}};
    options.auto_contrast = true;
    options.minimum_range = 24;
    anygear::d4xx::IrBgrStats stats;
    if (!expect(anygear::d4xx::ir_y8_to_bgr24(
                    narrow.data(), narrow.size(), 2, 2, 2,
                    options, &output, &stats),
                "narrow-range conversion failed") ||
        !expect(stats.low == 0 && stats.high == 255,
                "narrow range did not fall back to stable full-range mapping")) {
        return 1;
    }

    if (!expect(!anygear::d4xx::ir_y8_to_bgr24(
                    padded.data(), 3, 2, 2, 4,
                    options, &output),
                "truncated Y8 source was accepted")) {
        return 1;
    }
    std::fprintf(stderr, "PASS: D4xx IR to virtual BGR24 conversion\n");
    return 0;
}
