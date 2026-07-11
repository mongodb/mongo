// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_settings/query_settings_hash.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/query/query_settings/query_knob_overrides.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

#include <boost/container_hash/hash.hpp>
#include <fmt/format.h>

namespace mongo::query_settings {
namespace {
QuerySettingsKnobOverrides makeKnobOverrides(std::string_view jsonString) {
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

TEST(QuerySettingsHashTest, QuerySettingsHashIncludesQueryKnobs) {
    // Change queryKnobs in query settings, verify that the hash differs.
    QuerySettings settings;
    auto hashA = mongo::query_settings::hash(settings);

    settings.setQueryKnobs(makeKnobOverrides("{testIntKnobWire: 42}"));
    auto hashB = mongo::query_settings::hash(settings);

    settings.setQueryKnobs(makeKnobOverrides("{testBoolKnobWire: true}"));
    auto hashC = mongo::query_settings::hash(settings);

    ASSERT_NE(hashA, hashB);
    ASSERT_NE(hashB, hashC);
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
    ns.setColl(std::string_view("testColl"));
    settings.setIndexHints({{IndexHintSpec(ns, {IndexHint("a_1")})}});
    settings.setReject(true);
    auto observedHash = mongo::query_settings::hash(settings);

    static const size_t expectedHash = 0x7ed84960c1893d5c;

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

TEST(QuerySettingsKnobOverridesHashTest, HashReflectsKnobRemoval) {
    auto withKnob = makeKnobOverrides("{testIntKnobWire: 42}");
    auto nullOverride = makeKnobOverrides("{testIntKnobWire: null}");
    auto afterRemoval = QuerySettingsKnobOverrides::merge(withKnob, nullOverride);
    // merge() leaves the removal sentinel in place; simplify() strips it, as on the write path.
    afterRemoval.simplify();
    auto empty = QuerySettingsKnobOverrides::fromBSON(BSONObj{});
    boost::hash<QuerySettingsKnobOverrides> hasher;
    ASSERT_NE(hasher(withKnob), hasher(afterRemoval));
    ASSERT_EQ(hasher(afterRemoval), hasher(empty));
}

TEST(QuerySettingsKnobOverridesHashTest, HashStableForDifferentMergeInputOrder) {
    auto a = makeKnobOverrides("{testIntKnobWire: 3}");
    auto b = makeKnobOverrides("{testBoolKnobWire: false}");
    boost::hash<QuerySettingsKnobOverrides> hasher;
    ASSERT_EQ(hasher(QuerySettingsKnobOverrides::merge(a, b)),
              hasher(QuerySettingsKnobOverrides::merge(b, a)));
}

}  // namespace mongo::query_settings
