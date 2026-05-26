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

#include "mongo/db/query/query_settings/query_settings_hash.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/query/query_settings/query_knob_overrides.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/unittest/unittest.h"

#include <boost/container_hash/hash.hpp>
#include <fmt/format.h>

namespace mongo::query_settings {
namespace {
QuerySettingsKnobOverrides makeKnobOverrides(StringData jsonString) {
    return QuerySettingsKnobOverrides::fromBSON(fromjson(jsonString));
}
};  // namespace

TEST(QuerySettingsHashTest, QuerySettingsHashIncludesRejection) {
    // Change reject in query settings, verify that the hash differs.
    QuerySettings settings;
    auto hashA = mongo::query_settings::hash(settings);

    settings.setReject(false);
    auto hashB = mongo::query_settings::hash(settings);

    settings.setReject(true);
    auto hashC = mongo::query_settings::hash(settings);

    ASSERT_EQ(hashA, hashB);
    ASSERT_NE(hashA, hashC);
}

TEST(QuerySettingsHashTest, QuerySettingsHashExcludesComment) {
    // Change comment in query settings, verify that the hash does not differ.
    QuerySettings settings;
    settings.setQueryFramework(QueryFrameworkControlEnum::kForceClassicEngine);
    settings.setReject(true);

    auto hashA = mongo::query_settings::hash(settings);

    auto commentObj = BSON("reason for reject" << "don't want this query to be used on classic...");
    auto comment = Comment::parseFromBSON(commentObj.firstElement());
    settings.setComment(comment);
    auto hashB = mongo::query_settings::hash(settings);

    ASSERT_EQ(hashA, hashB);
}

TEST(QuerySettingsHashTest, QuerySettingsHashStability) {
    // Verify that the hash resulting from setting each query setting matches a "golden" value,
    // guarding stability of this hash.
    // Variation between versions is fine ()
    QuerySettings settings;
    settings.setQueryFramework(mongo::QueryFrameworkControlEnum::kForceClassicEngine);
    NamespaceSpec ns;
    ns.setDb(
        DatabaseNameUtil::deserialize(boost::none, "testDB", SerializationContext::stateDefault()));
    ns.setColl(StringData("testColl"));
    settings.setIndexHints({{IndexHintSpec(ns, {IndexHint("a_1")})}});
    settings.setReject(true);
    auto observedHash = mongo::query_settings::hash(settings);

    static const size_t expectedHash = 0xd14b8e06bcb187b;

    ASSERT_EQ(observedHash, expectedHash)
        << fmt::format("{:#016x} != {:#016x}", observedHash, expectedHash);
}

TEST(QuerySettingsKnobOverridesHashTest, HashDeterministicForEmpty) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSONObj{});
    boost::hash<QuerySettingsKnobOverrides> hasher;
    ASSERT_EQ(hasher(overrides), hasher(overrides));
}

TEST(QuerySettingsKnobOverridesHashTest, HashStableForIdentical) {
    auto a = makeKnobOverrides("{testIntKnobWire: 42}");
    auto b = makeKnobOverrides("{testIntKnobWire: 42}");
    boost::hash<QuerySettingsKnobOverrides> hasher;
    ASSERT_EQ(hasher(a), hasher(b));
}

TEST(QuerySettingsKnobOverridesHashTest, HashDiffersForDifferentValues) {
    auto a = makeKnobOverrides("{testIntKnobWire: 1}");
    auto b = makeKnobOverrides("{testIntKnobWire: 2}");
    boost::hash<QuerySettingsKnobOverrides> hasher;
    ASSERT_NE(hasher(a), hasher(b));
}

TEST(QuerySettingsKnobOverridesHashTest, HashDiffersForDifferentKnobs) {
    auto a = makeKnobOverrides("{testIntKnobWire: 1}");
    auto b = makeKnobOverrides("{testIntKnobWire: 1, testBoolKnobWire: false}");
    boost::hash<QuerySettingsKnobOverrides> hasher;
    ASSERT_NE(hasher(a), hasher(b));
}

TEST(QuerySettingsKnobOverridesHashTest, HashStableForDifferentKnobsOrder) {
    auto a = makeKnobOverrides("{testIntKnobWire: 7, testBoolKnobWire: false}");
    auto b = makeKnobOverrides("{testBoolKnobWire: false, testIntKnobWire: 7}");
    boost::hash<QuerySettingsKnobOverrides> hasher;
    ASSERT_EQ(hasher(a), hasher(b));
}

TEST(QuerySettingsKnobOverridesHashTest, HashStableForDuplicatedKnobDifferentOrder) {
    auto a = makeKnobOverrides("{testIntKnobWire: 4, testIntKnobWire: 2}");
    auto b = makeKnobOverrides("{testIntKnobWire: 2, testIntKnobWire: 4}");
    boost::hash<QuerySettingsKnobOverrides> hasher;
    ASSERT_EQ(hasher(a), hasher(b));
}

}  // namespace mongo::query_settings
