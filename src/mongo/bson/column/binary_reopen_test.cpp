/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/column/binary_reopen.h"

#include "mongo/base/data_view.h"
#include "mongo/bson/column/simple8b.h"
#include "mongo/bson/column/simple8b_builder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::bsoncolumn::internal {
using V = boost::optional<uint64_t>;

class BinaryReopenTest : public unittest::Test {
public:
    BinaryReopenTest();

    // Generates a BSONColumn control block for a set of simple8b blocks and optionally provided
    // scale factor
    const char* control(std::vector<uint64_t> blocks,
                        uint8_t scaleIndex = Simple8bTypeUtil::kMemoryAsInteger);

    // Simple8b block constants to be used in the tests
    uint64_t block1Skip;
    uint64_t block1Zero;
    uint64_t block1One;
    uint64_t block5Two;
    uint64_t block6Skip;
    uint64_t block60Skip;
    uint64_t block60Zero;
    uint64_t block2Zero1Skip;
    uint64_t block3One1Skip;
    uint64_t block3Skip1One;
    uint64_t block6Skip1Two;
    uint64_t blockFullOne;
    uint64_t block1RLE;
    uint64_t block16RLE;

private:
    // Memory for generated control blocks
    std::forward_list<std::unique_ptr<char[]>> _ownedControls;
};

BinaryReopenTest::BinaryReopenTest() {
    // Helper to generate a single simple8b block with the provided values.
    auto generateSimple8b = [](boost::optional<uint64_t> value,
                               int count,
                               boost::optional<uint64_t> value2 = boost::none,
                               int count2 = 0) {
        boost::optional<uint64_t> block;
        auto writeFn = [&](uint64_t b) mutable {
            if (block) {
                FAIL("Should only write one block");
            }
            block = b;
        };
        Simple8bBuilder<uint64_t> builder;
        for (int i = 0; i < count; i++) {
            if (value) {
                builder.append(*value, writeFn);
            } else {
                builder.skip(writeFn);
            }
        }
        for (int i = 0; i < count2; i++) {
            if (value2) {
                builder.append(*value2, writeFn);
            } else {
                builder.skip(writeFn);
            }
        }
        builder.flush(writeFn);
        ASSERT_TRUE(block.has_value());
        return *block;
    };

    // Helper to generate a simple8b block that can fit the maximum amount of a particular value
    auto generateFullSimple8b = [](boost::optional<uint64_t> value) {
        boost::optional<uint64_t> block;
        bool written = false;
        auto writeFn = [&](uint64_t b) mutable {
            block = b;
            written = true;
        };
        // We need to disable RLE, so we generate a previous value that is different from the value
        // we're appending.
        boost::optional<uint64_t> different = value ? V{*value + 1} : V{0};
        // Initialize RLE with this value
        Simple8bBuilder<uint64_t> builder(different, 0);
        // Append until a simple8b block has been full and written out
        while (!written) {
            if (value) {
                builder.append(*value, writeFn);
            } else {
                builder.skip(writeFn);
            }
        }
        return *block;
    };

    // Some constants used in the tests below
    block1Skip = generateSimple8b(boost::none, 1);
    block1Zero = generateSimple8b(0, 1);
    block1One = generateSimple8b(1, 1);
    block5Two = generateSimple8b(2, 5);
    block6Skip = generateSimple8b(boost::none, 6);
    block60Skip = generateSimple8b(boost::none, 60);
    block60Zero = generateSimple8b(0, 60);
    block2Zero1Skip = generateSimple8b(0, 2, boost::none, 1);
    block3One1Skip = generateSimple8b(1, 3, boost::none, 1);
    block3Skip1One = generateSimple8b(boost::none, 3, 1, 1);
    block6Skip1Two = generateSimple8b(boost::none, 6, 2, 1);
    blockFullOne = generateFullSimple8b(1);
    block1RLE = simple8b_internal::kRleSelector;
    block16RLE = simple8b_internal::kRleSelector | 0xF0;
}

const char* BinaryReopenTest::control(std::vector<uint64_t> blocks, uint8_t scaleIndex) {
    // A control block contains between 1 and 16 simple8b blocks.
    ASSERT_GT(blocks.size(), 0);
    ASSERT_LTE(blocks.size(), 16);

    // Allocate enough memory to also fit the control byte preceding the simple8b blocks.
    auto c = std::make_unique<char[]>(blocks.size() * sizeof(uint64_t) + 1);
    // Write control byte with out scale factor and number of simple8b blocks.
    *c.get() = kControlByteForScaleIndex[scaleIndex] | (blocks.size() - 1);
    // Copy simple8b data
    auto dv = DataView(c.get() + 1);
    for (size_t i = 0; i < blocks.size(); ++i) {
        dv.write<LittleEndian<uint64_t>>(blocks[i], i * sizeof(uint64_t));
    }
    auto ptr = c.get();
    // Store internally to simplify memory management in the tests
    _ownedControls.push_front(std::move(c));
    return ptr;
}

TEST_F(BinaryReopenTest, EstimateLastValue) {
    // Block with zeros return zero
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block1Zero})), V{0});

    // Skips before a value does not affect the last value
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block6Skip, block6Skip1Two})), V{2});

    // Block ending with skips returns last non-skip value
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block1Zero, block6Skip})), V{0});
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block5Two, block6Skip})), V{2});
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block3One1Skip, block6Skip})), V{1});

    // Block ending with 60 or more skips return none even if value exists before the skips
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block5Two, block60Skip})), V{boost::none});
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block5Two, block60Skip, block6Skip})),
              V{boost::none});
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block5Two,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip})),
              V{boost::none});

    // Block ending with 59 or fewer skips returns last non-skip value
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block5Two,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block1Skip,
                                                   block1Skip,
                                                   block1Skip,
                                                   block1Skip,
                                                   block1Skip})),
              V{2});

    // Block with skips only returns none
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip})),
              V{boost::none});
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block6Skip,
                                                   block1Skip,
                                                   block1Skip,
                                                   block1Skip,
                                                   block1Skip,
                                                   block1Skip})),
              V{boost::none});
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block1Skip})), V{boost::none});

    // Block with RLE returns zero regardless of what's before it
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block1RLE})), V{0});
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block1One, block1RLE})), V{0});
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block1Zero, block1RLE})), V{0});
    ASSERT_EQ(estimateLastValue<uint64_t>(control({block1Skip, block1RLE})), V{0});
}

TEST_F(BinaryReopenTest, FindOverflow) {
    OverflowResult<uint64_t> res;
    auto findOverflowHelper = [](const char* control, V lastVal) {
        Simple8bBuilder<uint64_t> detector(lastVal, 0);
        return findOverflow<uint64_t>(control, lastVal, detector);
    };

    // Basic case of a single simple8b block with skip does not overflow
    res = findOverflowHelper(control({block1Skip}), V{boost::none});
    ASSERT_EQ(res.overflowIndex, kInvalidIndex);
    ASSERT_EQ(res.lastValue, V{boost::none});  // last value is unchanged when there is no overflow
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // Basic case of a single simple8b block with values does not overflow
    res = findOverflowHelper(control({block5Two}), V{0});
    ASSERT_EQ(res.overflowIndex, kInvalidIndex);
    ASSERT_EQ(res.lastValue, V{0});  // last value is unchanged when there is no overflow
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // Two blocks with identical values does not overflow if there is a block that could have all
    // fit in
    res = findOverflowHelper(control({block5Two, block5Two}),
                             V{0});  // Different value for RLE disables RLE mode
    ASSERT_EQ(res.overflowIndex, kInvalidIndex);
    ASSERT_EQ(res.lastValue, V{0});  // last value is unchanged when there is no overflow
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // Two blocks with over 60 values that are different cannot fit in a single block so we overflow
    // at index 0.
    res = findOverflowHelper(control({block5Two, block60Zero}), V{0});
    ASSERT_EQ(res.overflowIndex, 0);
    ASSERT_EQ(res.lastValue, V{2});  // last value in block that overflowed
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // Three blocks with over 60 values that are different cannot fit in a single block so we
    // overflow at index 1.
    res = findOverflowHelper(control({block5Two, block5Two, block60Zero}), V{0});
    ASSERT_EQ(res.overflowIndex, 1);
    ASSERT_EQ(res.lastValue, V{2});  // last value in block that overflowed
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // Changing the last value does not affect the overflow point as RLE is not in play
    res = findOverflowHelper(control({block5Two, block5Two, block60Zero}), V{2});
    ASSERT_EQ(res.overflowIndex, 1);
    ASSERT_EQ(res.lastValue, V{2});  // last value in block that overflowed
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // Without RLE we can only fit 30 '1' values in a single block, so overflow happens at index
    // 1 even though values are identical
    res = findOverflowHelper(control({block5Two, blockFullOne, blockFullOne}), V{0});
    ASSERT_EQ(res.overflowIndex, 1);
    ASSERT_EQ(res.lastValue, V{1});  // last value in block that overflowed
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // With RLE overflow happens in first block with a different value
    res = findOverflowHelper(control({block5Two, blockFullOne, blockFullOne}), V{1});
    ASSERT_EQ(res.overflowIndex, 0);
    ASSERT_EQ(res.lastValue, V{2});  // last value in block that overflowed
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // No overflow if the values are all identical and RLE is in play
    res = findOverflowHelper(control({blockFullOne, blockFullOne}), V{1});
    ASSERT_EQ(res.overflowIndex, kInvalidIndex);
    ASSERT_EQ(res.lastValue, V{1});  // last value in block that overflowed
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // With RLE block and all values are identical the overflow happens before the RLE block
    res = findOverflowHelper(control({block5Two, block1RLE, block5Two}), V{2});
    ASSERT_EQ(res.overflowIndex, 0);
    ASSERT_EQ(res.lastValue, V{2});  // last value in block that overflowed
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // With RLE block and values are different before and after RLE the overflow happens at the RLE
    // block
    res = findOverflowHelper(control({blockFullOne, block1RLE, block5Two}), V{2});
    ASSERT_EQ(res.overflowIndex, 1);
    ASSERT_EQ(res.lastValue, V{1});  // last value in block that overflowed
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // With RLE block and values are different before and after RLE the overflow happens at the last
    // RLE block
    res = findOverflowHelper(control({blockFullOne, block16RLE, block1RLE, block5Two}), V{2});
    ASSERT_EQ(res.overflowIndex, 2);
    ASSERT_EQ(res.lastValue, V{1});  // last value in block that overflowed
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);

    // Only RLE returns no overflow but pending RLE at the last RLE block
    res = findOverflowHelper(control({block16RLE, block1RLE}), V{2});
    ASSERT_EQ(res.overflowIndex, kInvalidIndex);
    ASSERT_EQ(res.lastValue, V{2});  // last value in block that overflowed
    ASSERT_EQ(res.pendingRLEindex, 1);

    // RLE followed by non-RLE compatible with last value returns no overflow but pending RLE at
    // the last RLE block
    res = findOverflowHelper(control({block16RLE, block1RLE, block5Two}), V{2});
    ASSERT_EQ(res.overflowIndex, kInvalidIndex);
    ASSERT_EQ(res.lastValue, V{2});  // last value in block that overflowed
    ASSERT_EQ(res.pendingRLEindex, 1);

    // RLE followed by non-RLE not compatible with last value returns overflow at the
    // non-RLE block
    res = findOverflowHelper(control({block16RLE, block1RLE, block5Two}), V{1});
    ASSERT_EQ(res.overflowIndex, 1);
    ASSERT_EQ(res.lastValue,
              V{1});  // last value is left unchanged when it cannot be determined due to RLE
    ASSERT_EQ(res.pendingRLEindex, kInvalidIndex);
}

TEST_F(BinaryReopenTest, FindLastNonRLE) {
    LastNonRLEResult<uint64_t> res;

    // Single non-RLE returns index 0 and the last value in the block
    res = findLastNonRLE<uint64_t>(control({block1Zero}));
    ASSERT_EQ(res.index, 0);
    ASSERT_EQ(res.lastValue, V{0});

    // Single non-RLE returns index 0 and the last value in the block
    res = findLastNonRLE<uint64_t>(control({block2Zero1Skip}));
    ASSERT_EQ(res.index, 0);
    ASSERT_EQ(res.lastValue, V{boost::none});

    // Single non-RLE returns index 0 and the last value in the block
    res = findLastNonRLE<uint64_t>(control({block6Skip1Two}));
    ASSERT_EQ(res.index, 0);
    ASSERT_EQ(res.lastValue, V{2});

    // Multiple non-RLE blocks returns index to last block and the last value in that block
    res = findLastNonRLE<uint64_t>(control({blockFullOne, block6Skip1Two}));
    ASSERT_EQ(res.index, 1);
    ASSERT_EQ(res.lastValue, V{2});

    // RLE at the end is skipped. Position and last value to prior non-RLE block is returned
    res = findLastNonRLE<uint64_t>(control({blockFullOne, block1RLE}));
    ASSERT_EQ(res.index, 0);
    ASSERT_EQ(res.lastValue, V{1});

    // RLE at the end is skipped. Position and last value to prior non-RLE block is returned
    res = findLastNonRLE<uint64_t>(control({block1RLE, blockFullOne, block16RLE, block1RLE}));
    ASSERT_EQ(res.index, 1);
    ASSERT_EQ(res.lastValue, V{1});

    // Only RLE blocks returns invalid index and last value of 0
    res = findLastNonRLE<uint64_t>(control({block1RLE}));
    ASSERT_EQ(res.index, kInvalidIndex);
    ASSERT_EQ(res.lastValue, V{0});

    // Only RLE blocks returns invalid index and last value of 0
    res = findLastNonRLE<uint64_t>(control({block16RLE, block1RLE}));
    ASSERT_EQ(res.index, kInvalidIndex);
    ASSERT_EQ(res.lastValue, V{0});

    // Index parameter limits the search to before that index
    res =
        findLastNonRLE<uint64_t>(control({block6Skip1Two, blockFullOne, block16RLE, block1RLE}), 0);
    ASSERT_EQ(res.index, 0);
    ASSERT_EQ(res.lastValue, V{2});

    // Index parameter limits the search to before that index
    res = findLastNonRLE<uint64_t>(control({blockFullOne, block6Skip1Two, blockFullOne}), 1);
    ASSERT_EQ(res.index, 1);
    ASSERT_EQ(res.lastValue, V{2});

    // Index parameter limits the search to before that index
    res = findLastNonRLE<uint64_t>(
        control({blockFullOne, block6Skip1Two, block16RLE, block1RLE, blockFullOne}), 3);
    ASSERT_EQ(res.index, 1);
    ASSERT_EQ(res.lastValue, V{2});
}

TEST_F(BinaryReopenTest, Overflow) {
    // Helper to run the overflow detection on the OverflowState class
    auto overflowHelper = [](std::vector<const char*> controls) -> OverflowPoint<uint64_t> {
        ControlBlockContainer cbs;
        // Generate the control block container. We can ignore the data used for the double type
        // (checked in OverflowScaled below)
        for (auto&& c : controls) {
            cbs.push_back({c});
        }
        OverflowState<uint64_t> overflow(cbs.back());
        return overflow.detect(cbs);
    };

    // Helper for a control block full of RLE
    auto fullRLEControl = [&]() {
        return control({block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE,
                        block16RLE});
    };

    std::vector<const char*> controls;

    // Single control without overflow
    controls = {control({block5Two})};
    OverflowPoint<uint64_t> point = overflowHelper(controls);
    ASSERT_EQ(point.control(), controls[0]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{0});             // Last is defined as 0 when there is no overflow
    ASSERT_FALSE(point.allValuesIdentical());  // this is never set unless RLE is involved
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[0]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);

    // Single control with overflow at index 0
    controls = {control({block5Two, block60Zero})};
    point = overflowHelper(controls);
    ASSERT_EQ(point.control(), controls[0]);
    ASSERT_TRUE(point.overflow());
    ASSERT_EQ(point.index(), 0);
    ASSERT_EQ(point.last(), V{2});             // Last value in block that caused overflow
    ASSERT_FALSE(point.allValuesIdentical());  // this is never set unless RLE is involved
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[0]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);

    // Two controls with overflow in the first control at the second to last index position
    controls = {control({
                    block5Two,
                    block5Two,
                    block5Two,
                    block5Two,
                    block5Two,
                    block5Two,
                    block5Two,
                    block5Two,
                    block5Two,
                    block5Two,
                    block5Two,
                    block5Two,
                    block5Two,
                    block5Two,
                    blockFullOne,
                    block5Two,
                }),
                control({block5Two})};
    point = overflowHelper(controls);
    ASSERT_EQ(point.control(), controls[0]);
    ASSERT_TRUE(point.overflow());
    ASSERT_EQ(point.index(), 14);
    ASSERT_EQ(point.last(), V{1});             // Last value in block that caused overflow
    ASSERT_FALSE(point.allValuesIdentical());  // this is never set unless RLE is involved
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[0]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 1);

    // Two controls with overflow in the first control at last index position is treated as no
    // overflow with the second control returned.
    controls = {control({block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         block5Two,
                         blockFullOne}),
                control({block5Two})};
    point = overflowHelper(controls);
    ASSERT_EQ(point.control(), controls[1]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{1});             // Last value in block that caused overflow
    ASSERT_FALSE(point.allValuesIdentical());  // this is never set unless RLE is involved
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[1]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);

    // Single control with RLE only returns no overflow with last value of 0
    controls = {control({block1RLE})};
    point = overflowHelper(controls);
    ASSERT_EQ(point.control(), controls[0]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{0});  // Last is defined as 0 when there is no overflow
    ASSERT_TRUE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[0]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);

    // Only RLE can span more than one control which yields the same result
    controls = {fullRLEControl(), control({block16RLE, block1RLE})};
    point = overflowHelper(controls);
    ASSERT_EQ(point.control(), controls[0]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{0});  // Last is defined as 0 when there is no overflow
    ASSERT_TRUE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[0]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 1);

    // RLE spanning more than one control followed by blocks containing only zeros also yields the
    // same result
    controls = {fullRLEControl(),
                fullRLEControl(),
                fullRLEControl(),
                control({block16RLE, block1RLE, block60Zero})};
    point = overflowHelper(controls);
    ASSERT_EQ(point.control(), controls[0]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{0});  // Last is defined as 0 when there is no overflow
    ASSERT_TRUE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[0]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 3);

    // Value followed by RLE spanning more than one control is overflow at the index before the RLE
    // starts
    controls = {control({block5Two,
                         block5Two,
                         blockFullOne,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE}),
                control({block16RLE, block1RLE})};
    point = overflowHelper(controls);
    ASSERT_EQ(point.control(), controls[0]);
    ASSERT_TRUE(point.overflow());
    ASSERT_EQ(point.index(), 2);
    ASSERT_EQ(point.last(), V{1});  // Last value in block that caused overflow
    ASSERT_TRUE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[0]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 1);

    // Value followed by RLE spanning more than one control is overflow at the index before the
    // RLE starts as long as the value after RLE is the same as before RLE
    controls = {control({block5Two,
                         block5Two,
                         blockFullOne,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE}),
                control({block16RLE, block1RLE, block1One})};
    point = overflowHelper(controls);
    ASSERT_EQ(point.control(), controls[0]);
    ASSERT_TRUE(point.overflow());
    ASSERT_EQ(point.index(), 2);
    ASSERT_EQ(point.last(), V{1});  // Last value in block that caused overflow
    ASSERT_TRUE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[0]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 1);

    // When value before RLE is different from the value after RLE the overflow happens at the
    // last RLE block
    controls = {control({block5Two,
                         block5Two,
                         blockFullOne,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE}),
                fullRLEControl(),
                control({block16RLE, block1RLE, block5Two})};
    point = overflowHelper(controls);
    ASSERT_EQ(point.control(), controls[2]);
    ASSERT_TRUE(point.overflow());
    ASSERT_EQ(point.index(), 1);
    ASSERT_EQ(point.last(), V{1});  // Last value in block that caused overflow
    ASSERT_FALSE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[2]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);

    // When the stream starts with RLE but the value after RLE is not zero then the overflow happens
    // at the last RLE block
    controls = {fullRLEControl(), fullRLEControl(), control({block16RLE, block1RLE, block5Two})};
    point = overflowHelper(controls);
    ASSERT_EQ(point.control(), controls[2]);
    ASSERT_TRUE(point.overflow());
    ASSERT_EQ(point.index(), 1);
    ASSERT_EQ(point.last(), V{0});             // Last value in block that caused overflow
    ASSERT_FALSE(point.allValuesIdentical());  // this is never set unless RLE is involved
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[2]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);
}

TEST_F(BinaryReopenTest, OverflowScaled) {
    // Helper to run the overflow detection for doubles
    auto overflowHelper = [](double base,
                             std::vector<const char*> controls) -> OverflowPoint<uint64_t> {
        ControlBlockContainer cbs;

        // Every control block needs to set lastAtEndOfBlock. We calculate this based on 'base' and
        // the control blocks provided.
        uint64_t prevNonRLE = simple8b::kSingleZero;
        auto ret =
            Simple8bTypeUtil::encodeDouble(base, scaleIndexForControlByte(*controls.front()));
        ASSERT_TRUE(ret.has_value());
        int64_t encoded = *ret;

        for (auto&& c : controls) {
            uint8_t scaleIndex = scaleIndexForControlByte(*c);
            // Doubles uses delta encoding, so we can use a sum to get the delta for the last value.
            encoded += simple8b::sum<int64_t>(
                c + 1, numSimple8bBlocksForControlByte(*c) * sizeof(uint64_t), prevNonRLE);
            base = Simple8bTypeUtil::decodeDouble(encoded, scaleIndex);
            cbs.push_back({c, base, scaleIndex});
        }

        OverflowState<uint64_t> overflow(cbs.back());
        return overflow.detect(cbs);
    };

    std::vector<const char*> controls;

    // Rescale after full block is reported as no overflow in the first control with a different
    // scale
    controls = {control({blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne},
                        0),
                control({block5Two}, Simple8bTypeUtil::kMemoryAsInteger)};
    OverflowPoint<uint64_t> point = overflowHelper(1.0, controls);
    ASSERT_EQ(point.control(), controls[1]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{1});
    ASSERT_FALSE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[1]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);

    // Same but the last value before rescale is skip
    controls = {control({blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         blockFullOne,
                         block3One1Skip},
                        0),
                control({block5Two}, Simple8bTypeUtil::kMemoryAsInteger)};
    point = overflowHelper(1.0, controls);
    ASSERT_EQ(point.control(), controls[1]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{boost::none});
    ASSERT_FALSE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[1]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);

    // Same but there are RLE before the rescale
    controls = {control({blockFullOne,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE},
                        0),
                control({block5Two}, Simple8bTypeUtil::kMemoryAsInteger)};
    point = overflowHelper(1.0, controls);
    ASSERT_EQ(point.control(), controls[1]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{1});
    ASSERT_FALSE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[1]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);

    // Same but there are only RLE before the rescale
    controls = {control({block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE},
                        0),
                control({block5Two}, Simple8bTypeUtil::kMemoryAsInteger)};
    point = overflowHelper(1.0, controls);
    ASSERT_EQ(point.control(), controls[1]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{0});
    ASSERT_FALSE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[1]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);

    // RLE can be before and after the rescale
    controls = {control({blockFullOne,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE},
                        0),
                control({block16RLE}, Simple8bTypeUtil::kMemoryAsInteger)};
    point = overflowHelper(1.0, controls);
    ASSERT_EQ(point.control(), controls[1]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{1});
    ASSERT_FALSE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[1]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);

    // Block before rescale is not full but it is not possible to scale the first value with scale
    // factor kMemoryAsInteger with scale factor 0 so we also treat this as a no overflow but return
    // the first control after the rescale.
    controls = {control({blockFullOne, blockFullOne, blockFullOne}, 0),
                control({block5Two}, Simple8bTypeUtil::kMemoryAsInteger)};
    point = overflowHelper(1.0, controls);
    ASSERT_EQ(point.control(), controls[1]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{1});
    ASSERT_FALSE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[1]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);

    // Last value before rescale can be scaled with the next scale factor and all values fit in
    // pending without causing overflow. We then report the first control with a binary offset to
    // the control byte after the scaling.
    controls = {control({blockFullOne, blockFullOne, blockFullOne}, 1), control({block5Two}, 0)};
    point = overflowHelper(1.0, controls);
    ASSERT_EQ(point.control(), controls[0]);
    ASSERT_TRUE(point.overflow());
    ASSERT_EQ(point.index(), 2);
    ASSERT_EQ(point.last(), V{1});
    ASSERT_FALSE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[1]);
    ASSERT_EQ(point.lastControlOffset(),
              numSimple8bBlocksForControlByte(*controls[0]) * sizeof(uint64_t) + 1);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 1);

    // Like above but we have a large amount of RLE after the rescale. The result is basically the
    // same, but we report a larger offset and more values remaining.
    controls = {control({blockFullOne, blockFullOne, blockFullOne}, 1),
                control({block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE,
                         block16RLE},
                        0),
                control({block16RLE, block16RLE, blockFullOne}, 0)};
    point = overflowHelper(1.0, controls);
    ASSERT_EQ(point.control(), controls[0]);
    ASSERT_TRUE(point.overflow());
    ASSERT_EQ(point.index(), 2);
    ASSERT_EQ(point.last(), V{1});
    ASSERT_FALSE(point.allValuesIdentical());  // Values are not identical even if we have a large
                                               // amont of RLE because the scaling is different
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[2]);
    ASSERT_EQ(point.lastControlOffset(),
              (numSimple8bBlocksForControlByte(*controls[0]) + kMaxNumSimple8bPerControl) *
                      sizeof(uint64_t) +
                  2);  // binary offset to the third control byte
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 2);

    // Same but with RLE on both sides of the scaling
    controls = {control({blockFullOne, blockFullOne, block16RLE}, 1), control({block1RLE}, 0)};
    point = overflowHelper(1.0, controls);
    ASSERT_EQ(point.control(), controls[0]);
    ASSERT_TRUE(point.overflow());
    ASSERT_EQ(point.index(), 2);
    ASSERT_EQ(point.last(), V{1});
    ASSERT_FALSE(point.allValuesIdentical());  // Values are not identical even if we have a large
                                               // amont of RLE because the scaling is different
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[1]);
    ASSERT_EQ(point.lastControlOffset(),
              numSimple8bBlocksForControlByte(*controls[0]) * sizeof(uint64_t) +
                  1);  // binary offset to the second control byte
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 1);

    // Last value before rescale can be scaled with the next scale factor but all values cannot fit
    // in pending without causing overflow. This case is also treated as no overflow
    controls = {control({blockFullOne, blockFullOne, block5Two}, 1),
                control({blockFullOne, block1One}, 0)};
    point = overflowHelper(1.0, controls);
    ASSERT_EQ(point.control(), controls[1]);
    ASSERT_FALSE(point.overflow());
    ASSERT_EQ(point.index(), kInvalidIndex);
    ASSERT_EQ(point.last(), V{2});
    ASSERT_FALSE(point.allValuesIdentical());
    ASSERT_EQ(point.lastControl(), (uint8_t)*controls[1]);
    ASSERT_EQ(point.lastControlOffset(), 0);
    ASSERT_EQ(std::distance(point.remaining().begin(), point.remaining().end()), 0);
}
}  // namespace mongo::bsoncolumn::internal
