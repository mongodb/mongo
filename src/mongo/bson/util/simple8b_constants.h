/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <array>
#include <cstdint>

namespace mongo::simple8b_internal {
/*
 * Simple8B is a compression method for storing unsigned int 64 values. In this case
 * we make a few optimizations detailed below. We reserve the 4 lsbs for a baseSelector value. And
 * then we encode integers based on the following selector choice:
 *
 * Selector value:     0 |  1  2  3  4  5  6  7  8  9 10 11 12 13 14 | 15 (RLE)
 * Integers coded:     0 | 60 30 20 15 12 10  8  7  6  5  4  3  2  1 | up to 1920
 * Value Bits/integer: 0 |  1  2  3  4  5  6  7  8 10 12 15 20 30 60 | Last Value added
 * Wasted bits:        0 |  0  0  0  0  0  0  4  4  0  0  0  0  0  0 | 56
 * Total Bits/Integer: 0 |  1  2  3  4  5  6  7  8 10 12 15 20 30 60 | Last Valued added
 *
 * However, we make optimizations for selector value 7 and 8. We can see there are 4
 * wasted trailing bits. Using these 4 bits we can consider compression of trailing zeros.
 * For a selector extension value of 7, we store 4 bits and these represent up to 15 trailing zeros.
 * The extension bits are stored directly after the initial selector bits so that the simple8b word
 * looks like: | Base Selector (0-3) | Selector Extension (4-7) | Bits for Values (8 - 63)
 *
 * Selector Value:              0 | 7  7  7  7  7  7  7  7  7
 * Selector 7 Extension Value:  0 | 1  2  3  4  5  6  7  8  9
 * Value Bits/Integer:          0 | 2  3  4  5  7 10 14 24 52
 * TrailingZeroBits:            0 | 4  4  4  4  4  4  4  4  4
 * MaxTrailingZeroSize:         0 |15 15 15 15 15 15 15 15 15
 * Total Bits/Integer:          0 | 6  7  8  9 11 14 18 28 56
 *
 * Additionally, we consider larger trailing zero counts in selector 8. In this case the value
 * of the trailing zero bits is multiplied by a nibble shift of 4. We consider trailing zero sizes
 * of both 4 and 5 bits and thus, we split selector 8 in our implementation into Selector8Small and
 * Selector8Large
 *
 * Selector Value:             0 | 8   8   8   8   8   8   8   8   8   8   8   8   8
 * Selector 8 Extension Value: 0 | 1   2   3   4   5   6   7   8   9  10  11  12  13
 * Value Bits/Integer:         0 | 4   5   7  10  14  24  52   4   6   9  13  23  51
 * TrailingZeroBits:           0 | 4   4   4   4   4   4   4   5   5   5   5   5   5
 * MaxTrailingZerosSize:       0 |60  60  60  60  60  60  60 124 124 124 124 124 124
 * Total Bits/Integer:         0 | 8   9  11  14  18  28  56   9  11  14  18  28  56
 *
 * The simple8b words are according to this spec of selectors and their extension types.
 */

// Map selectorNames to their indexs.
static constexpr uint8_t kBaseSelector = 0;
static constexpr uint8_t kSevenSelector = 1;
static constexpr uint8_t kEightSelectorSmall = 2;
static constexpr uint8_t kEightSelectorLarge = 3;

// Variables to handle RLE
static constexpr uint8_t kRleSelector = 15;
static constexpr uint8_t kMaxRleCount = 16;
static constexpr uint8_t kRleMultiplier = 120;

// Mask to obtain the base and extended selectors.
static constexpr uint64_t kBaseSelectorMask = 0x000000000000000F;

// Selectors are always of size 4
static constexpr uint8_t kSelectorBits = 4;

// Nibble Shift is always of size 4
static constexpr uint8_t kNibbleShiftSize = 4;

// The max selector value for each extension
constexpr std::array<uint8_t, 4> kMaxSelector = {14, 9, 7, 13};

// The min selector value for each extension
constexpr std::array<uint8_t, 4> kMinSelector = {1, 1, 1, 8};

// The max amount of data bits each selector type can store. This is the amount of bits in the 64bit
// word that are not used for selector values.
constexpr std::array<uint8_t, 4> kDataBits = {60, 56, 56, 56};

// The amount of bits allocated to store a set of trailing zeros
constexpr std::array<uint8_t, 4> kTrailingZeroBitSize = {0, 4, 4, 5};

// The amount of possible trailing zeros each selector can handle in the trailingZeroBitSize
constexpr std::array<uint8_t, 4> kTrailingZerosMaxCount = {0, 15, 60, 124};

// Obtain a mask for the trailing zeros for the seven and eight selectors. We shift 4 and 5 bits to
// create the mask The trailingZeroBitSize variable is used as an index, but must be shifted - 4 to
// correspond to indexes 0 and 1.
constexpr std::array<uint8_t, 4> kTrailingZerosMask = {
    0, (1ull << 4) - 1, (1ull << 4) - 1, (1ull << 5) - 1};

// The amount of zeros each value in the trailing zero count represents
constexpr std::array<uint8_t, 4> kTrailingZerosMultiplier = {
    0, 1, kNibbleShiftSize, kNibbleShiftSize};

// Transfer from the base selector to the shift size.
constexpr std::array<uint8_t, 15> kBaseSelectorToShiftSize = {
    0, 0, 0, 0, 0, 0, 0, 4, 4, 0, 0, 0, 0, 0};

// Transfer from a selector to a specific extension type
// This is for selector 7 and 8 extensions where the selector value is passed along with
// selector index.
constexpr std::array<std::array<uint8_t, 16>, 2> kSelectorToExtension = {
    std::array<uint8_t, 16>{0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0},
    std::array<uint8_t, 16>{0, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 0, 0}};

// Transfer from a extensionType and selectorIdx to the selector value to be held in the 4 lsb (base
// selector)
constexpr std::array<std::array<uint8_t, 16>, 4> kExtensionToBaseSelector = {
    std::array<uint8_t, 16>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    std::array<uint8_t, 16>{7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7},
    std::array<uint8_t, 16>{8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8},
    std::array<uint8_t, 16>{8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8}};


// A mask to obtain the value bits for each selector including the trailing zero bits. The masks are
// calculated as the following: Mask = 2^(kBitsPerInteger+kTrailingZeroBitSize) - 1
constexpr std::array<std::array<uint64_t, 16>, 4> kDecodeMask = {
    std::array<uint64_t, 16>{0,
                             1,
                             (1ull << 2) - 1,
                             (1ull << 3) - 1,
                             (1ull << 4) - 1,
                             (1ull << 5) - 1,
                             (1ull << 6) - 1,
                             (1ull << 7) - 1,
                             (1ull << 8) - 1,
                             (1ull << 10) - 1,
                             (1ull << 12) - 1,
                             (1ull << 15) - 1,
                             (1ull << 20) - 1,
                             (1ull << 30) - 1,
                             (1ull << 60) - 1,
                             1},
    std::array<uint64_t, 16>{0,
                             (1ull << 6) - 1,
                             (1ull << 7) - 1,
                             (1ull << 8) - 1,
                             (1ull << 9) - 1,
                             (1ull << 11) - 1,
                             (1ull << 14) - 1,
                             (1ull << 18) - 1,
                             (1ull << 28) - 1,
                             (1ull << 56) - 1,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0},
    std::array<uint64_t, 16>{0,
                             (1ull << 8) - 1,
                             (1ull << 9) - 1,
                             (1ull << 11) - 1,
                             (1ull << 14) - 1,
                             (1ull << 18) - 1,
                             (1ull << 28) - 1,
                             (1ull << 56) - 1,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0},
    std::array<uint64_t, 16>{
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        (1ull << 9) - 1,
        (1ull << 11) - 1,
        (1ull << 14) - 1,
        (1ull << 18) - 1,
        (1ull << 28) - 1,
        (1ull << 56) - 1,
        0,
        0}};

// The number of meaningful bits for each selector. This does not include any trailing zero bits.
// We use 64 bits for all invalid selectors, this is to make sure iteration does not get stuck.
constexpr std::array<std::array<uint8_t, 16>, 4> kBitsPerIntForSelector = {
    std::array<uint8_t, 16>{64, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 15, 20, 30, 60, 64},
    std::array<uint8_t, 16>{64, 2, 3, 4, 5, 7, 10, 14, 24, 52, 64, 64, 64, 64, 64, 64},
    std::array<uint8_t, 16>{64, 4, 5, 7, 10, 14, 24, 52, 0, 0, 64, 64, 64, 64, 64, 64},
    std::array<uint8_t, 16>{64, 0, 0, 0, 0, 0, 0, 0, 4, 6, 9, 13, 23, 51, 64, 64}};

// The number of integers coded for each selector.
constexpr std::array<std::array<uint8_t, 16>, 4> kIntsStoreForSelector = {
    std::array<uint8_t, 16>{0, 60, 30, 20, 15, 12, 10, 8, 7, 6, 5, 4, 3, 2, 1, 0},
    std::array<uint8_t, 16>{0, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0},
    std::array<uint8_t, 16>{0, 7, 6, 5, 4, 3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    std::array<uint8_t, 16>{0, 0, 0, 0, 0, 0, 0, 0, 6, 5, 4, 3, 2, 1, 0, 0}};

}  // namespace mongo::simple8b_internal
