// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/field_stats/field_stats_builder.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/compiler/ce/field_stats/field_stats.h"
#include "mongo/unittest/unittest.h"

#include <array>
#include <cstdint>
#include <string>
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
    const auto uuid = UUID::gen();
    FieldStatsBuilder builder(paths, uuid);
    paths[0] = "other";

    const auto stats = builder.getStats();
    ASSERT_EQ(stats.get_id(), makeFieldStatsId(uuid, {"a.b"}));
}

TEST(FieldStatsBuilderTest, CanonicalizesPathsAndAlignsSketches) {
    const auto uuid = UUID::gen();
    FieldStatsBuilder builder(std::vector<std::string>{"b", "a"}, uuid);
    builder.accumulate(BSON("a" << 1 << "b" << 2));
    builder.accumulate(BSON("a" << 1 << "b" << 3));

    const auto stats = builder.getStats();
    ASSERT_EQ(stats.get_id(), makeFieldStatsId(uuid, {"a", "b"}));
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

TEST(FieldStatsBuilderTest, StartsWithNoDocumentObservations) {
    FieldStatsBuilder builder(std::vector<std::string>{"a"}, UUID::gen());
    const auto stats = builder.getStats();
    ASSERT_EQ(stats.getTotalDocs(), 0);
    ASSERT_EQ(stats.getNonScalarDocs(), 0);
    ASSERT_EQ(stats.getNullDocs(), 0);
    ASSERT_EQ(stats.getMissingDocs(), 0);
    ASSERT_EQ(stats.getUndefinedDocs(), 0);
}

TEST(FieldStatsBuilderTest, CountsDuplicateDocumentObservations) {
    FieldStatsBuilder builder(std::vector<std::string>{"a"}, UUID::gen());
    builder.accumulate(BSON("a" << 1));
    builder.accumulate(BSON("a" << 1));

    ASSERT_EQ(builder.getStats().getTotalDocs(), 2);
}

TEST(FieldStatsBuilderTest, SkipsFlaggedDocumentsWithoutSkippingObservations) {
    FieldStatsBuilder builder(std::vector<std::string>{"a.b", "c"}, UUID::gen());
    builder.accumulate(fromjson("{a: {b: 7}, c: 8}"));
    const auto before = builder.getStats();
    builder.accumulate(fromjson("{a: [{b: 1}, {b: 2}], c: 9}"));

    const auto stats = builder.getStats();
    ASSERT_BSONOBJ_BINARY_EQ(stats.getNdv().toBSON(), before.getNdv().toBSON());
    ASSERT_BSONOBJ_BINARY_EQ(stats.getMcv().toBSON(), before.getMcv().toBSON());
    ASSERT_EQ(stats.getTotalDocs(), 2);
}

struct DocumentCategoryCase {
    BSONObj document;
    std::vector<std::string> paths;
    // Non-scalar, null, missing, Undefined, in getter order below.
    std::array<std::int64_t, 4> counters;
};

class FieldStatsDocumentCategoryTest : public ::testing::TestWithParam<DocumentCategoryCase> {};

TEST_P(FieldStatsDocumentCategoryTest, CountsEachObservationInItsCategories) {
    const auto& param = GetParam();
    SCOPED_TRACE(param.document);
    FieldStatsBuilder builder(param.paths, UUID::gen());
    builder.accumulate(param.document);
    builder.accumulate(param.document);
    const auto stats = builder.getStats();
    ASSERT_EQ(stats.getTotalDocs(), 2);
    ASSERT_EQ((std::array{stats.getNonScalarDocs(),
                          stats.getNullDocs(),
                          stats.getMissingDocs(),
                          stats.getUndefinedDocs()}),
              param.counters);
}

INSTANTIATE_TEST_SUITE_P(
    DocumentCategories,
    FieldStatsDocumentCategoryTest,
    ::testing::Values(
        DocumentCategoryCase{BSON("a" << 1), {"a"}, {0, 0, 0, 0}},
        DocumentCategoryCase{BSON("a" << BSONNULL), {"a"}, {0, 2, 0, 0}},
        DocumentCategoryCase{BSONObj{}, {"a"}, {0, 0, 2, 0}},
        DocumentCategoryCase{BSON("a" << BSONUndefined), {"a"}, {0, 0, 0, 2}},
        DocumentCategoryCase{fromjson("{a: []}"), {"a"}, {2, 0, 0, 0}},
        DocumentCategoryCase{fromjson("{a: {}}"), {"a"}, {2, 0, 0, 0}},
        DocumentCategoryCase{BSON("a" << 1), {"a.b"}, {0, 0, 2, 0}},
        DocumentCategoryCase{BSON("a" << std::string(1024, 'x')), {"a"}, {0, 0, 0, 0}},
        DocumentCategoryCase{fromjson("{a: null, b: null}"), {"a", "b"}, {0, 2, 0, 0}},
        DocumentCategoryCase{
            BSON("a" << BSONUndefined << "b" << BSONUndefined), {"a", "b"}, {0, 0, 0, 2}},
        DocumentCategoryCase{BSONObj{}, {"a", "b"}, {0, 0, 2, 0}},
        DocumentCategoryCase{BSON("a" << BSONNULL), {"a", "b"}, {0, 2, 2, 0}},
        DocumentCategoryCase{
            BSON("a" << BSONNULL << "b" << BSONUndefined), {"a", "b"}, {0, 2, 0, 2}},
        DocumentCategoryCase{BSON("a" << BSONUndefined), {"a", "b"}, {0, 0, 2, 2}},
        DocumentCategoryCase{
            BSON("a" << BSONNULL << "b" << BSONUndefined), {"a", "b", "c"}, {0, 2, 2, 2}},
        DocumentCategoryCase{
            BSON("a" << BSONNULL << "b" << BSONUndefined << "c" << BSON_ARRAY(BSON("d" << 1))),
            {"a", "b", "c.e"},
            {2, 0, 0, 0}}));

TEST(FieldStatsBuilderTest, RetainsCategoryCountsAcrossDifferentObservations) {
    FieldStatsBuilder builder(std::vector<std::string>{"a", "b", "c"}, UUID::gen());
    builder.accumulate(BSON("a" << BSONNULL << "b" << BSONUndefined));
    builder.accumulate(BSON("a" << 1 << "b" << 2 << "c" << 3));
    builder.accumulate(fromjson("{a: [1]}"));

    const auto stats = builder.getStats();
    ASSERT_EQ(stats.getTotalDocs(), 3);
    ASSERT_EQ((std::array{stats.getNonScalarDocs(),
                          stats.getNullDocs(),
                          stats.getMissingDocs(),
                          stats.getUndefinedDocs()}),
              (std::array<std::int64_t, 4>{1, 1, 1, 1}));
}

TEST(FieldStatsBuilderTest, IncludesOversizedScalarValuesInNdv) {
    FieldStatsBuilder builder(std::vector<std::string>{"a"}, UUID::gen());
    builder.accumulate(BSON("a" << std::string(1024, 'x')));
    ASSERT_EQ(builder.getStats().getNdv().getSketches().front().getNdv(), 1);
}

TEST(FieldStatsBuilderTest, CountsDocumentsExceedingTupleLimitAsNonScalar) {
    FieldStatsBuilder builder(std::vector<std::string>{"a", "b", "c"}, UUID::gen());
    builder.accumulate(BSON("a" << 1 << "b" << 2 << "c" << 3));
    const auto before = builder.getStats();
    BSONArrayBuilder values;
    // 317 squared exceeds the 100,000-tuple extraction limit.
    values.appendNull();
    values.appendUndefined();
    for (int i = 2; i < 317; ++i) {
        values.append(i);
    }
    const auto array = values.arr();
    builder.accumulate(BSON("a" << array << "b" << array));
    const auto stats = builder.getStats();
    ASSERT_EQ(stats.getTotalDocs(), 2);
    ASSERT_EQ(stats.getNonScalarDocs(), 1);
    ASSERT_EQ(stats.getNullDocs(), 0);
    ASSERT_EQ(stats.getMissingDocs(), 0);
    ASSERT_EQ(stats.getUndefinedDocs(), 0);
    ASSERT_BSONOBJ_BINARY_EQ(stats.getNdv().toBSON(), before.getNdv().toBSON());
    ASSERT_BSONOBJ_BINARY_EQ(stats.getMcv().toBSON(), before.getMcv().toBSON());
}

}  // namespace
}  // namespace mongo::ce
