// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/ndv/ndv_sketch_builder.h"

#include "mongo/db/query/compiler/ce/ndv/ndv_hashing.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::ce {
namespace {

using sbe::value::TagValueView;

void assertNdvs(const NdvSketchBuilder& builder, const std::vector<long long>& expected) {
    std::vector<long long> actual;
    for (const auto& sketch : builder.getSketches()) {
        actual.push_back(sketch.getNdv());
    }
    ASSERT_EQ(actual, expected);
}

HyperLogLog deserializeSketch(const NdvSketch& sketch) {
    const auto registers = sketch.getRegisters();
    return uassertStatusOK(HyperLogLog::create(sketch.getPrecision(),
                                               {reinterpret_cast<const uint8_t*>(registers.data()),
                                                static_cast<size_t>(registers.length())}));
}

TEST(NdvSketchBuilderTest, StrictSketchUsesSharedHash) {
    const std::array values{TagValueView::numberInt32(7), TagValueView::null()};
    NdvSketchBuilder builder(2);
    builder.accumulateTuple(values);
    const auto strictSketch = deserializeSketch(builder.getSketches().front());

    auto expected = uassertStatusOK(HyperLogLog::create(NdvSketchBuilder::kPrecision));
    expected.addHash(hashValuesForNdv(values));

    ASSERT_TRUE(std::ranges::equal(strictSketch.registers(), expected.registers()));
}

TEST(NdvSketchBuilderTest, SerializedSketchesPreserveState) {
    const auto missing = TagValueView::nothing();
    const auto null = TagValueView::null();
    const auto one = TagValueView::numberInt32(1);

    NdvSketchBuilder builder(3);
    builder.accumulateTuple(std::array{missing, missing, one});
    builder.accumulateTuple(std::array{null, null, one});

    for (const auto& sketch : builder.getSketches()) {
        const auto restored = deserializeSketch(sketch);
        ASSERT_EQ(sketch.getNdv(), std::llround(restored.estimate()));
    }
}

TEST(NdvSketchBuilderTest, FoldingVariantsFollowFieldOrder) {
    const auto missing = TagValueView::nothing();
    const auto null = TagValueView::null();
    const auto one = TagValueView::numberInt32(1);
    const auto two = TagValueView::numberInt32(2);
    const auto three = TagValueView::numberInt32(3);

    const std::array tuples{
        // One missing/null pair in field 0.
        std::array{missing, one, one},
        std::array{null, one, one},
        // Two pairs in field 1.
        std::array{one, missing, one},
        std::array{one, null, one},
        std::array{two, missing, two},
        std::array{two, null, two},
        // Three pairs in field 2.
        std::array{one, one, missing},
        std::array{one, one, null},
        std::array{two, two, missing},
        std::array{two, two, null},
        std::array{three, three, missing},
        std::array{three, three, null},
    };

    NdvSketchBuilder builder(3);
    for (const auto& tuple : tuples) {
        builder.accumulateTuple(tuple);
    }

    // Strict, fold field 0, fold field 1, fold field 2, fold all missing fields.
    assertNdvs(builder, {12, 11, 10, 9, 6});
}

TEST(NdvSketchBuilderTest, AllMatchFoldsMultipleMissingFields) {
    const auto missing = TagValueView::nothing();
    const auto null = TagValueView::null();
    const auto one = TagValueView::numberInt32(1);

    const std::array tuplePairs{
        std::pair{std::array{missing, missing, one}, std::array{null, null, one}},
        std::pair{std::array{missing, missing, missing}, std::array{null, null, null}},
    };

    for (const auto& [missingTuple, nullTuple] : tuplePairs) {
        NdvSketchBuilder builder(3);
        builder.accumulateTuple(missingTuple);
        builder.accumulateTuple(nullTuple);

        ASSERT_EQ(builder.getSketches().back().getNdv(), 1);
    }
}

TEST(NdvSketchBuilderTest, AllMatchEqualsStrictWithoutMissingFields) {
    NdvSketchBuilder builder(2);
    builder.accumulateTuple(std::array{TagValueView::numberInt32(1), TagValueView::numberInt32(2)});

    const auto sketches = builder.getSketches();
    ASSERT_BSONOBJ_EQ(sketches.front().toBSON(), sketches.back().toBSON());
}

TEST(NdvSketchBuilderTest, UndefinedIsNotFoldedToNull) {
    const auto undefined = TagValueView{sbe::value::TypeTags::bsonUndefined, 0};
    const auto null = TagValueView::null();
    const auto one = TagValueView::numberInt32(1);

    NdvSketchBuilder builder(2);
    builder.accumulateTuple(std::array{undefined, one});
    builder.accumulateTuple(std::array{null, one});

    assertNdvs(builder, {2, 2, 2, 2});
}

TEST(NdvSketchBuilderTest, PreservesLegacyDbRefHashes) {
    using namespace std::literals::string_view_literals;

    const sbe::value::ObjectIdType id{};
    const auto dbRefWithNul = sbe::value::TagValueOwned::fromRaw(
        sbe::value::makeNewBsonDBPointer("db\0suffix"sv, id.data()));
    // Legacy Value conversion truncated the namespace to "db".
    const auto truncatedDbRef =
        sbe::value::TagValueOwned::fromRaw(sbe::value::makeNewBsonDBPointer("db", id.data()));

    const auto input = dbRefWithNul.view();
    const auto normalized = truncatedDbRef.view();
    const auto missing = TagValueView::nothing();

    const std::array tuplePairs{
        std::pair{std::vector{input}, std::vector{normalized}},
        std::pair{std::vector{input, missing}, std::vector{normalized, missing}},
        std::pair{std::vector{input, missing, input}, std::vector{normalized, missing, normalized}},
    };

    for (const auto& [inputTuple, normalizedTuple] : tuplePairs) {
        NdvSketchBuilder actual(inputTuple.size());
        actual.accumulateTuple(inputTuple);
        const auto actualSketches = actual.getSketches();

        NdvSketchBuilder expected(normalizedTuple.size());
        expected.accumulateTuple(normalizedTuple);
        const auto expectedSketches = expected.getSketches();

        ASSERT_EQ(actualSketches.size(), expectedSketches.size());
        for (size_t i = 0; i < actualSketches.size(); ++i) {
            ASSERT_BSONOBJ_EQ(actualSketches[i].toBSON(), expectedSketches[i].toBSON());
        }
    }
}

}  // namespace
}  // namespace mongo::ce
