// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/index_hints_serialization.h"

#include "mongo/bson/json.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string_view>

using namespace std::literals::string_view_literals;

using namespace std::literals::string_view_literals;
namespace mongo::query_settings::index_hints {

auto makeDbName(std::string_view dbName) {
    return DatabaseNameUtil::deserialize(
        boost::none /*tenantId=*/, dbName, SerializationContext::stateDefault());
}

TEST(IndexHintSpecsSerialization, TestSerialization) {
    IndexHintSpecs indexHints;
    {
        NamespaceSpec ns;
        ns.setDb(makeDbName("testDbA"));
        ns.setColl("testCollA"sv);
        indexHints.emplace_back(IndexHintSpec(ns, {IndexHint("a_1"), IndexHint("b_1")}));
    }
    {
        NamespaceSpec ns;
        ns.setDb(makeDbName("testDbB"));
        ns.setColl("testCollB"sv);
        indexHints.emplace_back(IndexHintSpec(
            ns, {IndexHint(NaturalOrderHint(NaturalOrderHint::Direction::kForward))}));
    }

    BSONObj indexHintSpecA = fromjson(R"({
      "ns": {
        "db": "testDbA",
        "coll": "testCollA"
      },
      "allowedIndexes": ["a_1", "b_1"]
    })");
    BSONObj indexHintSpecB = fromjson(R"({
      "ns": {
        "db": "testDbB",
        "coll": "testCollB"
      },
      "allowedIndexes": [{"$natural": 1}]
    })");
    BSONObj expectedOutput = BSON("indexHints" << BSON_ARRAY(indexHintSpecA << indexHintSpecB));

    {
        BSONObjBuilder bob;
        serialize(indexHints, "indexHints", &bob, SerializationContext::stateDefault());
        ASSERT_BSONOBJ_EQ(expectedOutput, bob.obj());
    }
}

TEST(IndexHintSpecsSerialization, TestDeserializationSingleSpec) {
    auto testDbName = "testDb";
    auto testCollName = "testColl"sv;
    auto dbName = makeDbName(testDbName);
    NamespaceSpec ns;
    ns.setDb(dbName);
    ns.setColl(testCollName);
    BSONObj obj = fromjson(R"({
      "ns": {
        "db": "testDb",
        "coll": "testColl"
      },
      "allowedIndexes": [{"$natural": -1}]
    })");
    auto parsedIndexHintSpecs = parse(boost::none /*tenantId=*/,
                                      BSON("" << obj).firstElement(),
                                      SerializationContext::stateDefault());

    IndexHintSpecs expectedHintSpecs{
        IndexHintSpec(ns, {IndexHint(NaturalOrderHint(NaturalOrderHint::Direction::kBackward))})};
    ASSERT_EQ(expectedHintSpecs, parsedIndexHintSpecs);
}

TEST(IndexHintSpecsSerialization, TestDeserializationMultipleSpecs) {
    auto testDbNameA = "testDbA";
    auto testCollNameA = "testCollA"sv;
    auto dbNameA = makeDbName(testDbNameA);
    NamespaceSpec nsA;
    nsA.setDb(dbNameA);
    nsA.setColl(testCollNameA);

    auto testDbNameB = "testDbB";
    auto testCollNameB = "testCollB"sv;
    auto dbNameB = makeDbName(testDbNameB);
    NamespaceSpec nsB;
    nsB.setDb(dbNameB);
    nsB.setColl(testCollNameB);

    BSONObj indexHintSpecA = fromjson(R"({
      "ns": {
        "db": "testDbA",
        "coll": "testCollA"
      },
      "allowedIndexes": [{"$natural": 1}]
    })");

    BSONObj indexHintSpecB = fromjson(R"({
      "ns": {
        "db": "testDbB",
        "coll": "testCollB"
      },
      "allowedIndexes": [{a: 1}, {b: 1}]
    })");
    BSONArray array = BSON_ARRAY(indexHintSpecA << indexHintSpecB);
    auto parsedIndexHintSpecs = parse(boost::none /*tenantId=*/,
                                      BSON("" << array).firstElement(),
                                      SerializationContext::stateDefault());

    IndexHintSpecs expectedHintSpecs{
        IndexHintSpec(nsA, {IndexHint(NaturalOrderHint(NaturalOrderHint::Direction::kForward))}),
        IndexHintSpec(nsB, {IndexHint(BSON("a" << 1)), IndexHint(BSON("b" << 1))})};
    ASSERT_EQ(expectedHintSpecs, parsedIndexHintSpecs);
}

TEST(IndexHintSpecsSerialization, TestFailedDeserialization) {
    BSONObj invalidIndexHintsSpecs;
    ASSERT_THROWS_CODE(parse(boost::none /*tenantId=*/,
                             BSON("" << invalidIndexHintsSpecs).firstElement(),
                             SerializationContext::stateDefault()),
                       DBException,
                       ErrorCodes::IDLFailedToParse);
    ASSERT_THROWS_CODE(parse(boost::none /*tenantId=*/,
                             BSON("" << 1).firstElement(),
                             SerializationContext::stateDefault()),
                       DBException,
                       ErrorCodes::FailedToParse);
}
}  // namespace mongo::query_settings::index_hints
