// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/field_stats/mcv_sketch_builder.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/unittest.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace mongo::ce {
namespace {

using sbe::value::TagValueView;

void accumulateTuple(McvSketchBuilder& builder, const BSONObj& tuple) {
    std::vector<TagValueView> values;
    for (const auto& element : tuple) {
        values.push_back(sbe::bson::convertToView(element));
    }
    builder.accumulate(values);
}

BSONObj makeEntry(const BSONObj& value, long long estFreq = 1) {
    return BSON("value" << value.firstElement() << "estFreq" << estFreq << "err" << 0LL);
}

void assertSketches(const McvStats& stats,
                    const std::vector<std::vector<BSONObj>>& expectedSketches) {
    const auto& sketches = stats.getSketches();
    ASSERT_EQ(sketches.size(), expectedSketches.size());

    for (size_t fieldIndex = 0; fieldIndex < sketches.size(); ++fieldIndex) {
        SCOPED_TRACE(fieldIndex);

        const auto& entries = sketches[fieldIndex].getEntries();
        const auto& expectedEntries = expectedSketches[fieldIndex];
        ASSERT_EQ(entries.size(), expectedEntries.size());

        for (size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
            SCOPED_TRACE(entryIndex);

            ASSERT_BSONOBJ_BINARY_EQ(entries[entryIndex].toBSON(), expectedEntries[entryIndex]);
        }
    }
}

TEST(McvSketchBuilderTest, SkipsNullishValuesWithoutShiftingOtherFields) {
    const std::array nullishValues{TagValueView::nothing(),
                                   TagValueView::null(),
                                   TagValueView{sbe::value::TypeTags::bsonUndefined, 0}};
    for (const auto value : nullishValues) {
        SCOPED_TRACE(value);
        McvSketchBuilder builder(3);
        builder.accumulate(
            std::array{TagValueView::numberInt32(1), value, TagValueView::numberInt32(2)});
        assertSketches(builder.getStats(),
                       {{makeEntry(BSON("value" << 1))}, {}, {makeEntry(BSON("value" << 2))}});
    }
}

TEST(McvSketchBuilderTest, CountersAreIndependentPerField) {
    McvSketchBuilder builder(2);
    accumulateTuple(builder, BSON("a" << 1 << "b" << 1));
    accumulateTuple(builder, BSON("a" << 1 << "b" << 2));
    accumulateTuple(builder, BSON("a" << 1 << "b" << 2));
    assertSketches(builder.getStats(),
                   {{makeEntry(BSON("value" << 1), 3)},
                    {makeEntry(BSON("value" << 1)), makeEntry(BSON("value" << 2), 2)}});
}

TEST(McvSketchBuilderTest, SkipsOversizedValuesWithoutShiftingOtherFields) {
    const std::string oversized(1024, 'x');
    const auto retained = BSON("value" << std::string(1000, 'x'));
    McvSketchBuilder builder(3);
    accumulateTuple(builder, BSON("a" << 1 << "b" << oversized << "c" << 2));
    accumulateTuple(builder, BSON("a" << 1 << "b" << retained.firstElement() << "c" << 2));
    assertSketches(builder.getStats(),
                   {{makeEntry(BSON("value" << 1), 2)},
                    {makeEntry(retained)},
                    {makeEntry(BSON("value" << 2), 2)}});
}

TEST(McvSketchBuilderTest, CombinesNumericallyEqualValuesAcrossTypes) {
    McvSketchBuilder builder(1);
    accumulateTuple(builder, BSON("a" << 7));
    accumulateTuple(builder, BSON("a" << 7LL));
    assertSketches(builder.getStats(), {{makeEntry(BSON("value" << 7), 2)}});
}

TEST(McvSketchBuilderTest, CapsCountersPerField) {
    McvSketchBuilder builder(1);
    for (int i = 0; i < 101; ++i) {
        accumulateTuple(builder, BSON("a" << i));
    }
    ASSERT_EQ(builder.getStats().getSketches()[0].getEntries().size(), 100u);
}

TEST(McvSketchBuilderTest, MismatchedTupleSizesLeaveExistingSketchesUnchanged) {
    for (const auto& tuple : {BSON("a" << 3), BSON("a" << 3 << "b" << 4 << "c" << 5)}) {
        SCOPED_TRACE(tuple);

        McvSketchBuilder builder(2);
        accumulateTuple(builder, BSON("a" << 1 << "b" << 2));

        const auto before = builder.getStats().toBSON();
        ASSERT_TASSERT_CODE(accumulateTuple(builder, tuple), 13794504);
        ASSERT_BSONOBJ_BINARY_EQ(builder.getStats().toBSON(), before);
    }
}

}  // namespace
}  // namespace mongo::ce
