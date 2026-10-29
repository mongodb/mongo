// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/match_expression_test_util.h"

#include <algorithm>

namespace mongo {

std::vector<char> bitPositionsToBinData(std::vector<uint32_t> positions) {
    if (positions.empty()) {
        return {};
    }
    const auto maxBit = *std::max_element(positions.begin(), positions.end());
    std::vector<char> buf(maxBit / 8 + 1, 0);
    for (auto pos : positions) {
        buf[pos / 8] |= static_cast<char>(1u << (pos % 8));
    }
    return buf;
}

}  // namespace mongo
