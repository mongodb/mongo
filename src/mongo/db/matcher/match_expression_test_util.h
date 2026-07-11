// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <cstdint>
#include <vector>

namespace mongo {

/**
 * Converts a list of bit positions to a minimal-length BinData byte buffer.
 * Bit N is stored in byte N/8 at position N%8 (bit 0 = LSB of byte 0), matching
 * MongoDB's convention for BinData in bit-test expressions.
 */
std::vector<char> bitPositionsToBinData(std::vector<uint32_t> positions);

}  // namespace mongo
