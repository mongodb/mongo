// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/field_stats/field_stats_builder.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <string_view>
#include <vector>

namespace mongo::ce {
namespace {

TEST(FieldStatsBuilderTest, RejectsEmptyFieldPathList) {
    ASSERT_THROWS_CODE(
        FieldStatsBuilder(std::vector<std::string>{}, UUID::gen()), DBException, 13176201);
}

TEST(FieldStatsBuilderTest, RejectsTooManyFieldPaths) {
    const std::vector<std::string> paths{"a", "b", "c", "d"};
    ASSERT_THROWS_CODE(FieldStatsBuilder(paths, UUID::gen()), DBException, 13176201);
}

TEST(FieldStatsBuilderTest, RejectsDuplicateFieldPaths) {
    const std::vector<std::string> paths{"a", "a"};
    ASSERT_THROWS_CODE(FieldStatsBuilder(paths, UUID::gen()), DBException, 13176202);
}

TEST(FieldStatsBuilderTest, RejectsOverlappingFieldPathsInReverseOrder) {
    const std::vector<std::string> paths{"a.b", "a"};
    ASSERT_THROWS_CODE(FieldStatsBuilder(paths, UUID::gen()), DBException, 13176203);
}

TEST(FieldStatsBuilderTest, RejectsNonAdjacentOverlappingFieldPaths) {
    const std::vector<std::string> paths{"a", "a-b", "a.b"};
    ASSERT_THROWS_CODE(FieldStatsBuilder(paths, UUID::gen()), DBException, 13176203);
}

TEST(FieldStatsBuilderTest, OwnsFieldPaths) {
    std::vector<std::string> paths{"a.b"};
    FieldStatsBuilder builder(paths, UUID::gen());
    paths[0] = "other";

    const auto stats = builder.getStats();
    ASSERT_EQ(stats.getSortedFieldPaths(), (std::vector<std::string_view>{"a.b"}));
}

TEST(FieldStatsBuilderTest, CanonicalizesPathsAndAlignsSketches) {
    FieldStatsBuilder builder(std::vector<std::string>{"b", "a"}, UUID::gen());
    builder.accumulate(BSON("a" << 1 << "b" << 2));
    builder.accumulate(BSON("a" << 1 << "b" << 3));

    const auto stats = builder.getStats();
    ASSERT_EQ(stats.getSortedFieldPaths(), (std::vector<std::string_view>{"a", "b"}));
    ASSERT_EQ(stats.getNdv().getSketches().front().getNdv(), 2);

    const auto& sketches = stats.getMcv().getSketches();
    ASSERT_EQ(sketches.size(), 2u);
    ASSERT_EQ(sketches[0].getEntries().size(), 1u);
    ASSERT_BSONELT_EQ(sketches[0].getEntries()[0].getValue().getElement(),
                      BSON("value" << 1).firstElement());
    ASSERT_EQ(sketches[0].getEntries()[0].getEstFreq(), 2);
    ASSERT_EQ(sketches[1].getEntries().size(), 2u);
    ASSERT_BSONELT_EQ(sketches[1].getEntries()[0].getValue().getElement(),
                      BSON("value" << 2).firstElement());
    ASSERT_BSONELT_EQ(sketches[1].getEntries()[1].getValue().getElement(),
                      BSON("value" << 3).firstElement());
    ASSERT_EQ(sketches[1].getEntries()[0].getEstFreq(), 1);
    ASSERT_EQ(sketches[1].getEntries()[1].getEstFreq(), 1);
}

TEST(FieldStatsBuilderTest, CountsDuplicateDocumentObservations) {
    FieldStatsBuilder builder(std::vector<std::string>{"a"}, UUID::gen());
    builder.accumulate(BSON("a" << 1));
    builder.accumulate(BSON("a" << 1));

    ASSERT_EQ(builder.getNumDocumentsObserved(), 2u);
}

TEST(FieldStatsBuilderTest, SkipsFlaggedDocumentsWithoutSkippingObservations) {
    FieldStatsBuilder builder(std::vector<std::string>{"a.b", "c"}, UUID::gen());
    builder.accumulate(fromjson("{a: {b: 7}, c: 8}"));
    const auto before = builder.getStats();
    builder.accumulate(fromjson("{a: [{b: 1}, {b: 2}], c: 9}"));

    const auto stats = builder.getStats();
    ASSERT_BSONOBJ_BINARY_EQ(stats.getNdv().toBSON(), before.getNdv().toBSON());
    ASSERT_BSONOBJ_BINARY_EQ(stats.getMcv().toBSON(), before.getMcv().toBSON());
    ASSERT_EQ(builder.getNumDocumentsObserved(), 2u);
}

}  // namespace
}  // namespace mongo::ce
