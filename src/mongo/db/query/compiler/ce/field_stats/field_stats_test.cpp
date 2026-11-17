// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/field_stats/field_stats.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/query/compiler/ce/field_stats/field_stats_gen.h"
#include "mongo/db/query/compiler/ce/field_stats/field_stats_test_util.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mongo::ce {
namespace {

TEST(FieldStatsIdTest, PinsIdFormat) {
    // $merge and _id lookups compare the id byte for byte, so the format must never change.
    const UUID uuid = UUID::gen();
    ASSERT_EQ(
        makeFieldStatsId(uuid, {"a.b"}),
        std::string(str::stream() << kFieldStatsSchemaVersion << "|" << uuid.toString() << "|a.b"));
}

TEST(FieldStatsIdTest, CanonicalizesFieldPathOrder) {
    const UUID uuid = UUID::gen();
    ASSERT_EQ(makeFieldStatsId(uuid, {"b", "a"}), makeFieldStatsId(uuid, {"a", "b"}));
}

TEST(FieldStatsIdTest, EscapesSeparatorsInFieldPaths) {
    // '|' and '\' are legal in field names; escaping keeps the id injective.
    const UUID uuid = UUID::gen();
    ASSERT_NE(makeFieldStatsId(uuid, {"a|b"}), makeFieldStatsId(uuid, {"a", "b"}));
    ASSERT_NE(makeFieldStatsId(uuid, {"a\\|b"}), makeFieldStatsId(uuid, {"a|b"}));
}

TEST(FieldStatsIdTest, PinsCompositeIdFormat) {
    // Composite ids join the sorted paths with the same separator.
    const UUID uuid = UUID::gen();
    ASSERT_EQ(makeFieldStatsId(uuid, {"c", "a.b"}),
              std::string(str::stream()
                          << kFieldStatsSchemaVersion << "|" << uuid.toString() << "|a.b|c"));
}

TEST(FieldStatsIdTest, CompositeIdsStayInjectiveUnderEscaping) {
    // A '|' inside one path of a composite tuple must not collide with a differently split
    // tuple of the same characters.
    const UUID uuid = UUID::gen();
    ASSERT_NE(makeFieldStatsId(uuid, {"a", "b|c"}), makeFieldStatsId(uuid, {"a", "b", "c"}));
    ASSERT_NE(makeFieldStatsId(uuid, {"a|b", "c"}), makeFieldStatsId(uuid, {"a", "b|c"}));
}

FieldStatsDoc makeStatsDoc() {
    const UUID uuid = uassertStatusOK(UUID::parse("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));

    NdvSketch sketch;
    sketch.setNdv(3);
    sketch.setRegisters(std::vector<std::uint8_t>{0, 1, 2});
    sketch.setPrecision(14);
    NdvStats ndvStats;
    ndvStats.setSketches({std::move(sketch)});

    FieldStatsDoc doc;
    doc.set_id(makeFieldStatsId(uuid, {"a.b"}));
    doc.setSchemaVersion(kFieldStatsSchemaVersion);
    doc.setCollectedAt(Date_t::fromMillisSinceEpoch(1000));
    doc.setTotalDocs(10);
    doc.setNonScalarDocs(2);
    doc.setNullDocs(1);
    doc.setMissingDocs(3);
    doc.setUndefinedDocs(1);
    doc.setNdv(std::move(ndvStats));

    McvEntry entry;
    entry.setValue(IDLAnyTypeOwned(BSON("value" << 1.5).firstElement()));
    entry.setEstFreq(5);
    entry.setErr(2);
    McvSketch mcvSketch;
    mcvSketch.setEntries({std::move(entry)});
    McvStats mcvStats;
    mcvStats.setSketches({std::move(mcvSketch)});
    doc.setMcv(std::move(mcvStats));

    return doc;
}

TEST(FieldStatsDocTest, SerializationGolden) {
    const auto doc = makeStatsDoc();

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "_id": "1|aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee|a.b",
            "schemaVersion": 1,
            "collectedAt": {"$date":"1970-01-01T00:00:01.000Z"},
            "totalDocs": 10,
            "nonScalarDocs": 2,
            "nullDocs": 1,
            "missingDocs": 3,
            "undefinedDocs": 1,
            "ndv": {
                "sketches": [
                    {
                        "ndv": 3,
                        "registers": {"$binary":{"base64":"AAEC","subType":"0"}},
                        "precision": 14
                    }
                ]
            },
            "mcv": {
                "sketches": [
                    {
                        "entries": [
                            {
                                "value": 1.5,
                                "estFreq": 5,
                                "err": 2
                            }
                        ]
                    }
                ]
            }
        })",
        doc.toBSON());
}

TEST(FieldStatsDocTest, McvEntriesRejectInvalidCounts) {
    for (const auto& entry : {BSON("value" << BSONObj() << "estFreq" << 0LL << "err" << 0LL),
                              BSON("value" << BSONObj() << "estFreq" << 1LL << "err" << -1LL)}) {
        ASSERT_THROWS_CODE(McvEntry::parse(entry, IDLParserContext("McvEntry")),
                           DBException,
                           ErrorCodes::BadValue);
    }
}

TEST(FieldStatsDocTest, RoundTripsThroughIdl) {
    const auto doc = makeStatsDoc();

    const BSONObj serialized = doc.toBSON();
    const auto parsed = FieldStatsDoc::parse(serialized, IDLParserContext("FieldStatsDoc"));
    // Serialize -> parse -> serialize must be the identity.
    ASSERT_BSONOBJ_EQ(parsed.toBSON(), serialized);
    ASSERT_EQ(parsed.get_id(), doc.get_id());
    ASSERT_EQ(parsed.getSchemaVersion(), kFieldStatsSchemaVersion);
    ASSERT_EQ(parsed.getNdv().getSketches().size(), 1u);
    ASSERT_EQ(parsed.getNdv().getSketches().front().getNdv(), 3);
}

TEST(SingleFieldStatsValidateTest, ApproximateEstimatesAreNotBounded) {
    // The NDV and the MCV entries come from approximate sketches, so the statistics are not
    // rejected when they disagree with each other or with the document counts.
    ASSERT_OK(SingleFieldStats{}.validate());
    // An NDV above totalDocsNum; the conversion from a document caps it.
    ASSERT_OK(SingleFieldStats({.ndv = 110, .totalDocsNum = 100}).validate());
    // More MCV entries than the NDV estimate.
    ASSERT_OK(SingleFieldStats(
                  {.mcv = mcvWithIntKeys(3, 1), .ndv = 2, .totalDocsNum = 100, .headDocsNum = 3})
                  .validate());
    // 10 - 2 = 8 remaining distinct values, but only 100 - 94 = 6 residual documents.
    ASSERT_OK(SingleFieldStats(
                  {.mcv = mcvWithIntKeys(2, 47), .ndv = 10, .totalDocsNum = 100, .headDocsNum = 94})
                  .validate());
    // A negative NDV is rejected.
    ASSERT_NOT_OK(SingleFieldStats({.ndv = -1, .totalDocsNum = 100}).validate());
}

TEST(SingleFieldStatsValidateTest, CountersExceedingTotalDocsIsInvalid) {
    // 10 + 20 + 25 + 5 + 40 = 100 documents fit exactly; one more non-scalar or undefined
    // document does not.
    SingleFieldStats stats{.mcv = mcvWithIntKeys(2, 20),
                           .ndv = 5,
                           .nonScalarDocsNum = 10,
                           .nullDocsNum = 20,
                           .missingDocsNum = 25,
                           .undefinedDocsNum = 5,
                           .totalDocsNum = 100,
                           .headDocsNum = 40};
    ASSERT_OK(stats.validate());
    stats.nonScalarDocsNum = 11;
    ASSERT_EQ(stats.validate().code(), ErrorCodes::BadValue);
    stats.nonScalarDocsNum = 10;
    stats.undefinedDocsNum = 6;
    ASSERT_EQ(stats.validate().code(), ErrorCodes::BadValue);
}

TEST(SingleFieldStatsTest, FindMcv) {
    // A lookup compares numbers across type tags, like an equality predicate: the double 1.0
    // is found by the int32 1.
    std::vector<std::pair<stats::SBEValue, McvEntryStats>> entries;
    entries.emplace_back(stats::SBEValue{stats::makeDoubleValue(1.0)}, McvEntryStats{5, 1});
    const SingleFieldStats stats{.mcv = mcvMap(std::move(entries)), .ndv = 3, .totalDocsNum = 10};
    const auto [oneTag, oneVal] = stats::makeInt32Value(1);
    const auto* entry = stats.findMcv(oneTag, oneVal);
    ASSERT(entry);
    ASSERT_EQ(entry->estFreq, 5U);
    ASSERT_EQ(entry->err, 1U);
    // A value that is not monitored is not found.
    const auto [twoTag, twoVal] = stats::makeInt32Value(2);
    ASSERT_FALSE(stats.findMcv(twoTag, twoVal));
}

TEST(SingleFieldStatsTest, ResidualDocsNum) {
    const SingleFieldStats stats{.nonScalarDocsNum = 10,
                                 .nullDocsNum = 20,
                                 .missingDocsNum = 30,
                                 .undefinedDocsNum = 5,
                                 .totalDocsNum = 100,
                                 .headDocsNum = 15};
    ASSERT_EQ(stats.residualDocsNum(), 20.0);
    // Counters exceeding totalDocsNum give a residual of 0, not a negative value.
    const SingleFieldStats inconsistent{.totalDocsNum = 10, .headDocsNum = 20};
    ASSERT_EQ(inconsistent.residualDocsNum(), 0.0);
}

TEST(SingleFieldStatsTest, ResidualNdv) {
    SingleFieldStats stats{.mcv = mcvWithIntKeys(3, 10), .ndv = 10, .totalDocsNum = 100};
    ASSERT_EQ(stats.residualNdv(), 7.0);
    // Null and missing each take one distinct value, regardless of their document counts.
    stats.nullDocsNum = 5;
    ASSERT_EQ(stats.residualNdv(), 6.0);
    stats.missingDocsNum = 8;
    ASSERT_EQ(stats.residualNdv(), 5.0);
    // Missing and undefined hash equally in the NDV sketch, so together they take one value.
    stats.undefinedDocsNum = 3;
    ASSERT_EQ(stats.residualNdv(), 5.0);
    // Undefined alone takes one distinct value.
    stats.missingDocsNum = 0;
    ASSERT_EQ(stats.residualNdv(), 5.0);
    // More MCV entries than the NDV estimate gives 0, not a negative value.
    const SingleFieldStats moreMcvThanNdv{
        .mcv = mcvWithIntKeys(3, 1), .ndv = 2, .totalDocsNum = 100};
    ASSERT_EQ(moreMcvThanNdv.residualNdv(), 0.0);
}

TEST(CompoundFieldStatsValidateTest, Shape) {
    // n >= 2 fields have n + 2 NDVs, for n up to kMaxFieldStatsFields.
    ASSERT_OK(CompoundFieldStats({.ndvs = {10, 9, 8, 7}}).validate());
    ASSERT_OK(CompoundFieldStats({.ndvs = {10, 9, 8, 7, 6}}).validate());
    ASSERT_NOT_OK(CompoundFieldStats{}.validate());
    ASSERT_NOT_OK(CompoundFieldStats({.ndvs = {10}}).validate());
    ASSERT_NOT_OK(CompoundFieldStats({.ndvs = {10, 9, 8}}).validate());
    ASSERT_NOT_OK(CompoundFieldStats({.ndvs = {10, 9, 8, 7, 6, 5}}).validate());
    ASSERT_NOT_OK(CompoundFieldStats({.ndvs = {10, 9, -1, 7}}).validate());
}

TEST(CompoundFieldStatsCapTest, CapsEachNdvAtTotalDocs) {
    CompoundFieldStats stats{.ndvs = {110, 105, 80, 95}, .totalDocsNum = 100};
    stats.capNdvs();
    ASSERT_EQ(stats.ndvs[0], 100.0);
    ASSERT_EQ(stats.ndvs[1], 100.0);
    ASSERT_EQ(stats.ndvs[2], 80.0);
    ASSERT_EQ(stats.ndvs[3], 95.0);
}

TEST(CompoundFieldStatsCapTest, PerFieldNdvMayExceedStrictNdv) {
    // The documents {b: 1} and {a: undefined, b: 1}: in the strict sketch missing and undefined
    // hash equally, so (missing, 1) and (undefined, 1) are one tuple. Counting the missing 'a' as
    // null gives (null, 1) and (undefined, 1), two tuples. Capping keeps these NDVs.
    CompoundFieldStats stats{.ndvs = {1, 2, 1, 2}, .totalDocsNum = 2};
    stats.capNdvs();
    ASSERT_EQ(stats.ndvs[0], 1.0);
    ASSERT_EQ(stats.ndvs[1], 2.0);
    ASSERT_EQ(stats.ndvs[2], 1.0);
    ASSERT_EQ(stats.ndvs[3], 2.0);
}

// The document counters of a statistics document.
struct DocCounters {
    std::int64_t totalDocs = 1000;
    std::int64_t nonScalarDocs = 0;
    std::int64_t nullDocs = 0;
    std::int64_t missingDocs = 0;
    std::int64_t undefinedDocs = 0;
};

NdvSketch makeNdvSketch(std::int64_t ndv) {
    NdvSketch sketch;
    sketch.setNdv(ndv);
    sketch.setRegisters(std::vector<std::uint8_t>{});
    sketch.setPrecision(14);
    return sketch;
}

FieldStatsDoc makeSingleFieldDoc(std::vector<McvEntry> entries,
                                 std::int64_t ndv,
                                 const DocCounters& counters = {}) {
    FieldStatsDoc doc;
    doc.set_id(makeFieldStatsId(UUID::gen(), {"a"}));
    doc.setSchemaVersion(kFieldStatsSchemaVersion);
    doc.setCollectedAt(Date_t::now());
    doc.setTotalDocs(counters.totalDocs);
    doc.setNonScalarDocs(counters.nonScalarDocs);
    doc.setNullDocs(counters.nullDocs);
    doc.setMissingDocs(counters.missingDocs);
    doc.setUndefinedDocs(counters.undefinedDocs);
    NdvStats ndvStats;
    ndvStats.setSketches({makeNdvSketch(ndv)});
    doc.setNdv(std::move(ndvStats));
    std::vector<McvSketch> mcvSketches;
    mcvSketches.emplace_back(std::move(entries));
    doc.setMcv(McvStats(std::move(mcvSketches)));
    return doc;
}

McvEntry makeMcvEntry(const BSONObj& holder, std::int64_t estFreq, std::int64_t err) {
    McvEntry entry;
    entry.setValue(IDLAnyTypeOwned(holder.firstElement(), holder));
    entry.setEstFreq(estFreq);
    entry.setErr(err);
    return entry;
}

TEST(MakeFieldStatsFromDocTest, ConvertsSingleFieldDoc) {
    auto stats = makeSingleFieldStatsFromDoc(makeSingleFieldDoc(
        {makeMcvEntry(BSON("" << 1), 100, 0), makeMcvEntry(BSON("" << "x"), 50, 10)},
        20,
        {.totalDocs = 1000,
         .nonScalarDocs = 5,
         .nullDocs = 7,
         .missingDocs = 3,
         .undefinedDocs = 2}));
    ASSERT_OK(stats.getStatus());
    ASSERT_EQ(stats.getValue().ndv, 20.0);
    ASSERT_EQ(stats.getValue().mcv.size(), 2U);
    ASSERT_EQ(stats.getValue().headDocsNum, 140U);
    ASSERT_EQ(stats.getValue().totalDocsNum, 1000U);
    ASSERT_EQ(stats.getValue().nonScalarDocsNum, 5U);
    ASSERT_EQ(stats.getValue().nullDocsNum, 7U);
    ASSERT_EQ(stats.getValue().missingDocsNum, 3U);
    ASSERT_EQ(stats.getValue().undefinedDocsNum, 2U);

    const auto [tag, val] = stats::makeInt32Value(1);
    const auto* entry = stats.getValue().findMcv(tag, val);
    ASSERT(entry);
    ASSERT_EQ(entry->estFreq, 100U);
}

// Returns a statistics document over 'fieldPaths' with one NDV sketch per value of 'ndvs'.
FieldStatsDoc makeCompositeDoc(std::vector<std::string> fieldPaths,
                               std::vector<std::int64_t> ndvs,
                               const DocCounters& counters = {}) {
    auto doc = makeSingleFieldDoc({}, 1, counters);
    std::vector<McvSketch> mcvSketches(fieldPaths.size());
    doc.set_id(makeFieldStatsId(UUID::gen(), std::move(fieldPaths)));
    std::vector<NdvSketch> sketches;
    for (const auto ndv : ndvs) {
        sketches.push_back(makeNdvSketch(ndv));
    }
    NdvStats ndvStats;
    ndvStats.setSketches(std::move(sketches));
    doc.setNdv(std::move(ndvStats));
    doc.setMcv(McvStats(std::move(mcvSketches)));
    return doc;
}

TEST(MakeFieldStatsFromDocTest, ConvertsCompoundDoc) {
    // Three fields persist 5 NDV sketches, kept in sketch order. Of the document counters only
    // totalDocs is carried over.
    auto stats = makeCompoundFieldStatsFromDoc(
        makeCompositeDoc({"a", "b", "c"},
                         {50, 48, 49, 47, 45},
                         {.totalDocs = 100, .nonScalarDocs = 4, .nullDocs = 6, .missingDocs = 8}),
        3);
    ASSERT_OK(stats.getStatus());
    ASSERT_EQ(stats.getValue().totalDocsNum, 100U);
    ASSERT_EQ(stats.getValue().ndvs.size(), 5U);
    ASSERT_EQ(stats.getValue().ndvs[0], 50.0);
    ASSERT_EQ(stats.getValue().ndvs[1], 48.0);
    ASSERT_EQ(stats.getValue().ndvs[2], 49.0);
    ASSERT_EQ(stats.getValue().ndvs[3], 47.0);
    ASSERT_EQ(stats.getValue().ndvs[4], 45.0);
}

TEST(MakeFieldStatsFromDocTest, CapsNdvs) {
    // The single and compound conversions cap separately; CompoundFieldStatsCapTest covers the
    // capped compound values.
    auto single = makeSingleFieldStatsFromDoc(makeSingleFieldDoc({}, 200, {.totalDocs = 100}));
    ASSERT_OK(single.getStatus());
    ASSERT_EQ(single.getValue().ndv, 100.0);
    auto compound = makeCompoundFieldStatsFromDoc(
        makeCompositeDoc({"a", "b"}, {200, 150, 39, 50}, {.totalDocs = 100}), 2);
    ASSERT_OK(compound.getStatus());
    ASSERT_EQ(compound.getValue().ndvs[0], 100.0);
}

TEST(MakeFieldStatsFromDocTest, RejectsInvalidDocs) {
    // A compound document covers 2 to kMaxFieldStatsFields fields.
    const auto twoFields = makeCompositeDoc({"a", "b"}, {40, 38, 39, 37});
    ASSERT_NOT_OK(makeCompoundFieldStatsFromDoc(twoFields, 1).getStatus());
    ASSERT_NOT_OK(makeCompoundFieldStatsFromDoc(
                      makeCompositeDoc({"a", "b", "c", "d"}, {50, 48, 49, 47, 46, 45}), 4)
                      .getStatus());

    // A compound document needs n + 2 NDV sketches for the requested n fields.
    ASSERT_NOT_OK(makeCompoundFieldStatsFromDoc(twoFields, 3).getStatus());
    // A single-field document has one NDV and one MCV sketch.
    ASSERT_NOT_OK(makeSingleFieldStatsFromDoc(twoFields).getStatus());

    // An err above estFreq is not a valid MCV entry.
    ASSERT_NOT_OK(
        makeSingleFieldStatsFromDoc(makeSingleFieldDoc({makeMcvEntry(BSON("" << 1), 5, 6)}, 1))
            .getStatus());
    // Each conversion checks its result with validate().
    ASSERT_NOT_OK(makeCompoundFieldStatsFromDoc(makeCompositeDoc({"a", "b"}, {40, -1, 39, 37}), 2)
                      .getStatus());
    ASSERT_NOT_OK(
        makeSingleFieldStatsFromDoc(
            makeSingleFieldDoc({}, 1, {.totalDocs = 100, .nullDocs = 60, .undefinedDocs = 50}))
            .getStatus());
}

class FieldStatsCounterTest : public ::testing::TestWithParam<const char*> {};

TEST_P(FieldStatsCounterTest, RejectsNegativeCount) {
    const auto bson = makeStatsDoc()
                          .toBSON()
                          .removeField(GetParam())
                          .addField(BSON(GetParam() << -1LL).firstElement());
    ASSERT_THROWS_CODE(FieldStatsDoc::parse(bson, IDLParserContext("FieldStatsDoc")),
                       DBException,
                       ErrorCodes::BadValue);
}

INSTANTIATE_TEST_SUITE_P(
    DocumentCounters,
    FieldStatsCounterTest,
    ::testing::Values("totalDocs", "nonScalarDocs", "nullDocs", "missingDocs", "undefinedDocs"));

class FieldStatsRequiredFieldTest : public ::testing::TestWithParam<const char*> {};

TEST_P(FieldStatsRequiredFieldTest, RejectsMissingField) {
    ASSERT_THROWS_CODE(FieldStatsDoc::parse(makeStatsDoc().toBSON().removeField(GetParam()),
                                            IDLParserContext("FieldStatsDoc")),
                       DBException,
                       ErrorCodes::IDLFailedToParse);
}

INSTANTIATE_TEST_SUITE_P(RequiredFields,
                         FieldStatsRequiredFieldTest,
                         ::testing::Values("_id",
                                           "schemaVersion",
                                           "collectedAt",
                                           "totalDocs",
                                           "nonScalarDocs",
                                           "nullDocs",
                                           "missingDocs",
                                           "undefinedDocs",
                                           "ndv",
                                           "mcv"));

}  // namespace
}  // namespace mongo::ce
