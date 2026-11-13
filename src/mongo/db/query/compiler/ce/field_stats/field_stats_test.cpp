// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/field_stats/field_stats.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/query/compiler/ce/field_stats/field_stats_gen.h"
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
