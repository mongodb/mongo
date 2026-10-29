// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/ndv/field_stats.h"

#include "mongo/db/query/compiler/ce/ndv/field_stats_gen.h"
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

TEST(FieldStatsDocTest, SerializationGolden) {
    // Deterministic inputs so the golden below pins the exact persisted format; any schema
    // change breaks this test visibly.
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
    doc.setCollectionUuid(uuid);
    doc.setSortedFieldPaths(std::vector<std::string>{"a.b"});
    doc.setCreatedAt(Date_t::fromMillisSinceEpoch(1000));
    doc.setNdv(std::move(ndvStats));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "_id": "1|aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee|a.b",
            "schemaVersion": 1,
            "collectionUuid": {"$uuid":"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"},
            "sortedFieldPaths": [
                "a.b"
            ],
            "createdAt": {"$date":"1970-01-01T00:00:01.000Z"},
            "ndv": {
                "sketches": [
                    {
                        "ndv": 3,
                        "registers": {"$binary":{"base64":"AAEC","subType":"0"}},
                        "precision": 14
                    }
                ]
            }
        })",
        doc.toBSON());
}

TEST(FieldStatsDocTest, RoundTripsThroughIdl) {
    const UUID uuid = UUID::gen();

    NdvSketch sketch;
    sketch.setNdv(3);
    sketch.setRegisters(std::vector<std::uint8_t>(1 << 14));
    sketch.setPrecision(14);
    NdvStats ndvStats;
    ndvStats.setSketches({std::move(sketch)});

    FieldStatsDoc doc;
    doc.set_id(makeFieldStatsId(uuid, {"a.b"}));
    doc.setSchemaVersion(kFieldStatsSchemaVersion);
    doc.setCollectionUuid(uuid);
    doc.setSortedFieldPaths(std::vector<std::string>{"a.b"});
    doc.setCreatedAt(Date_t::now());
    doc.setNdv(std::move(ndvStats));

    const BSONObj serialized = doc.toBSON();
    const auto parsed = FieldStatsDoc::parse(serialized, IDLParserContext("FieldStatsDoc"));
    // Serialize -> parse -> serialize must be the identity.
    ASSERT_BSONOBJ_EQ(parsed.toBSON(), serialized);
    ASSERT_EQ(parsed.get_id(), makeFieldStatsId(uuid, {"a.b"}));
    ASSERT_EQ(parsed.getCollectionUuid(), uuid);
    ASSERT_EQ(parsed.getSchemaVersion(), kFieldStatsSchemaVersion);
    ASSERT_EQ(parsed.getSortedFieldPaths().size(), 1u);
    ASSERT_EQ(parsed.getSortedFieldPaths().front(), "a.b");
    ASSERT_EQ(parsed.getNdv().getSketches().size(), 1u);
    ASSERT_EQ(parsed.getNdv().getSketches().front().getNdv(), 3);
}

}  // namespace
}  // namespace mongo::ce
