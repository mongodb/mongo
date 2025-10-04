/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/query_settings/index_hints_serialization.h"

#include "mongo/bson/json.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::query_settings::index_hints {

auto makeDbName(StringData dbName) {
    return DatabaseNameUtil::deserialize(
        boost::none /*tenantId=*/, dbName, SerializationContext::stateDefault());
}

TEST(IndexHintSpecsSerialization, TestSerialization) {
    IndexHintSpecs indexHints;
    {
        NamespaceSpec ns;
        ns.setDb(makeDbName("testDbA"));
        ns.setColl("testCollA"_sd);
        indexHints.emplace_back(IndexHintSpec(ns, {IndexHint("a_1"), IndexHint("b_1")}));
    }
    {
        NamespaceSpec ns;
        ns.setDb(makeDbName("testDbB"));
        ns.setColl("testCollB"_sd);
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
    auto testCollName = "testColl"_sd;
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
    auto testCollNameA = "testCollA"_sd;
    auto dbNameA = makeDbName(testDbNameA);
    NamespaceSpec nsA;
    nsA.setDb(dbNameA);
    nsA.setColl(testCollNameA);

    auto testDbNameB = "testDbB";
    auto testCollNameB = "testCollB"_sd;
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
