// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/resolved_namespace.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {
namespace {

TimeseriesOptions makeTimeseriesOptions(std::string_view timeField) {
    TimeseriesOptions opts;
    opts.setTimeField(timeField);
    return opts;
}

BSONObj serializeToObj(const TimeseriesViewMetadata& meta) {
    BSONObjBuilder b;
    meta.serialize(&b);
    return b.obj();
}

BSONObj serializeToObj(const ResolvedNamespace& rn) {
    BSONObjBuilder builder;
    rn.serialize(&builder);
    return builder.obj();
}

TEST(TimeseriesViewMetadataSerializeTest, TimeseriesOptionsNestedInBuilder) {
    TimeseriesViewMetadata meta;
    meta.options = makeTimeseriesOptions("t");
    BSONObj result = serializeToObj(meta);
    // timeseriesOptions must be a sub-object directly in the builder output.
    ASSERT(result.hasField(TimeseriesViewMetadata::kTimeseriesOptions));
    ASSERT_EQ(result[TimeseriesViewMetadata::kTimeseriesOptions].type(), BSONType::object);
    ASSERT_EQ(result[TimeseriesViewMetadata::kTimeseriesOptions].Obj()["timeField"].str(), "t");
}

TEST(TimeseriesViewMetadataSerializeTest, NoTimeseriesOptionsFieldWhenAbsent) {
    TimeseriesViewMetadata meta;
    BSONObj result = serializeToObj(meta);
    ASSERT_FALSE(result.hasField(TimeseriesViewMetadata::kTimeseriesOptions));
}

TEST(TimeseriesViewMetadataSerializeTest, OptionalBoolFieldsSerializeWhenSet) {
    TimeseriesViewMetadata meta;
    meta.options = makeTimeseriesOptions("t");
    meta.mayContainMixedData = false;
    meta.usesExtendedRange = true;
    meta.fixedBuckets = true;
    BSONObj result = serializeToObj(meta);
    ASSERT_FALSE(result[TimeseriesViewMetadata::kTimeseriesMayContainMixedData].boolean());
    ASSERT_TRUE(result[TimeseriesViewMetadata::kTimeseriesUsesExtendedRange].boolean());
    ASSERT_TRUE(result[TimeseriesViewMetadata::kTimeseriesfixedBuckets].boolean());
}

TEST(TimeseriesViewMetadataSerializeTest, MayContainMixedDataOmittedWhenTrue) {
    // Only serialized when explicitly false (mixed data absent means clean collection).
    TimeseriesViewMetadata meta;
    meta.mayContainMixedData = true;
    BSONObj result = serializeToObj(meta);
    ASSERT_FALSE(result.hasField(TimeseriesViewMetadata::kTimeseriesMayContainMixedData));
}

TEST(ResolvedNamespaceSerializeTest, TimeseriesOptionsNestedInsideResolvedView) {
    // The key structural assertion from SERVER-125735: after SERVER-120269 fixed the
    // BSONObjBuilder misuse, timeseriesOptions is correctly a sub-field of resolvedView.
    const auto ns = NamespaceString::createNamespaceString_forTest("db.coll");
    ResolvedNamespaceViewOptions opts;
    opts.timeseriesMetadata = TimeseriesViewMetadata{.options = makeTimeseriesOptions("t")};
    ResolvedNamespace rn(ns, ns, {}, {}, opts);
    auto result = serializeToObj(rn);
    ASSERT(result.hasField("resolvedView"));
    BSONObj resolvedView = result["resolvedView"].Obj();
    // timeseriesOptions must be inside resolvedView, not at the top level.
    ASSERT(resolvedView.hasField(TimeseriesViewMetadata::kTimeseriesOptions));
    ASSERT_FALSE(result.hasField(TimeseriesViewMetadata::kTimeseriesOptions));
}

TEST(ResolvedNamespaceSerializeTest, RoundTripWithTimeseriesMetadata) {
    const auto ns = NamespaceString::createNamespaceString_forTest("db.coll");
    ResolvedNamespaceViewOptions opts;
    opts.timeseriesMetadata = TimeseriesViewMetadata{
        .options = makeTimeseriesOptions("t"),
        .mayContainMixedData = false,
        .usesExtendedRange = true,
        .fixedBuckets = true,
    };
    ResolvedNamespace original(ns, ns, {}, {}, opts);
    auto serialized = serializeToObj(original);
    ResolvedNamespace roundTripped = ResolvedNamespace::fromBSON(serialized);
    ASSERT(roundTripped.isTimeseries());
    // getTimeseriesViewMetadata() returns by value — store it to avoid dangling refs.
    auto tsMeta = roundTripped.getTimeseriesViewMetadata();
    ASSERT(tsMeta.has_value());
    ASSERT(tsMeta->options.has_value());
    ASSERT_EQ(tsMeta->options->getTimeField(), "t");
    ASSERT(tsMeta->mayContainMixedData.has_value());
    ASSERT_FALSE(*tsMeta->mayContainMixedData);
    ASSERT(tsMeta->usesExtendedRange.has_value());
    ASSERT_TRUE(*tsMeta->usesExtendedRange);
    ASSERT(tsMeta->fixedBuckets.has_value());
    ASSERT_TRUE(*tsMeta->fixedBuckets);
}

TEST(ResolvedNamespaceSerializeTest, RoundTripWithoutTimeseriesMetadata) {
    const auto ns = NamespaceString::createNamespaceString_forTest("db.coll");
    ResolvedNamespace original(ns, {}, boost::none, false);
    auto serialized = serializeToObj(original);
    ResolvedNamespace roundTripped = ResolvedNamespace::fromBSON(serialized);
    // fromBSON always materialises a TimeseriesViewMetadata struct, but the namespace
    // is not a timeseries because no timeseriesOptions sub-object was present.
    ASSERT_FALSE(roundTripped.isTimeseries());
    auto tsMeta = roundTripped.getTimeseriesViewMetadata();
    ASSERT(tsMeta.has_value());
    ASSERT_FALSE(tsMeta->options.has_value());
}

TEST(ResolvedNamespaceSerializeTest, SerializesAndParsesAdditionalResolvedNamespaces) {
    ResolvedNamespace primary(NamespaceString::createNamespaceString_forTest("db.primary"),
                              NamespaceString::createNamespaceString_forTest("db.primary"),
                              {BSON("$match" << BSON("a" << 1))},
                              BSONObj());
    primary.setAdditionalResolvedNamespaces({
        ResolvedNamespace(NamespaceString::createNamespaceString_forTest("db.view1"),
                          NamespaceString::createNamespaceString_forTest("db.coll1"),
                          {BSON("$match" << BSON("b" << 2))},
                          BSONObj()),
        ResolvedNamespace(NamespaceString::createNamespaceString_forTest("otherDb.view2"),
                          NamespaceString::createNamespaceString_forTest("otherDb.coll2"),
                          {BSON("$project" << BSON("c" << 1))},
                          BSONObj()),
    });

    auto serialized = serializeToObj(primary);
    ResolvedNamespace parsed = ResolvedNamespace::fromBSON(serialized);
    ASSERT_EQ(parsed.getAdditionalResolvedNamespaces().size(), 2u);

    const auto& rn0 = parsed.getAdditionalResolvedNamespaces()[0];
    ASSERT_EQ(rn0.getNamespace().ns_forTest(), "db.view1");
    ASSERT_EQ(rn0.getResolvedNamespace().ns_forTest(), "db.coll1");
    ASSERT_EQ(rn0.getBsonPipeline().size(), 1u);
    ASSERT_BSONOBJ_EQ(rn0.getBsonPipeline()[0], BSON("$match" << BSON("b" << 2)));

    const auto& rn1 = parsed.getAdditionalResolvedNamespaces()[1];
    ASSERT_EQ(rn1.getNamespace().ns_forTest(), "otherDb.view2");
    ASSERT_EQ(rn1.getResolvedNamespace().ns_forTest(), "otherDb.coll2");
    ASSERT_EQ(rn1.getBsonPipeline().size(), 1u);
    ASSERT_BSONOBJ_EQ(rn1.getBsonPipeline()[0], BSON("$project" << BSON("c" << 1)));
}

TEST(ResolvedNamespaceSerializeTest, AdditionalResolvedNamespacesOmittedWhenEmpty) {
    const auto ns = NamespaceString::createNamespaceString_forTest("db.primary");
    ResolvedNamespace rn(ns, ns, {BSON("$match" << BSON("a" << 1))}, BSONObj());
    auto obj = serializeToObj(rn);
    ASSERT_FALSE(obj["resolvedView"].Obj().hasField("additionalResolvedNamespaces"));
}

TEST(ResolvedNamespaceSerializeTest, AdditionalResolvedNamespacesPreservesCollation) {
    const auto ns = NamespaceString::createNamespaceString_forTest("db.primary");
    ResolvedNamespace primary(ns, ns, {BSON("$match" << BSON("a" << 1))}, BSONObj());
    primary.setAdditionalResolvedNamespaces({
        ResolvedNamespace(NamespaceString::createNamespaceString_forTest("db.localizedView"),
                          NamespaceString::createNamespaceString_forTest("db.coll"),
                          {BSON("$match" << BSON("x" << 1))},
                          BSON("locale" << "fr_CA")),
    });

    ResolvedNamespace parsed = ResolvedNamespace::fromBSON(serializeToObj(primary));
    ASSERT_EQ(parsed.getAdditionalResolvedNamespaces().size(), 1u);
    ASSERT_BSONOBJ_EQ(parsed.getAdditionalResolvedNamespaces()[0].getDefaultCollation(),
                      BSON("locale" << "fr_CA"));
}

TEST(ResolvedNamespaceSerializeTest, AdditionalResolvedNamespacesRejectsNonObjectEntries) {
    const auto nss = NamespaceString::createNamespaceString_forTest("db.coll");
    BSONObj serialized =
        BSON("resolvedView" << BSON("ns" << nss.ns_forTest() << "pipeline" << BSONArray()
                                         << "additionalResolvedNamespaces"
                                         << BSON_ARRAY("notAnObject")));
    ASSERT_THROWS_CODE(ResolvedNamespace::fromBSON(serialized), AssertionException, 12451401);
}

TEST(ResolvedNamespaceSerializeTest, AdditionalResolvedNamespacesOmitsUserNsWhenEqualToResolvedNs) {
    const auto ns = NamespaceString::createNamespaceString_forTest("db.primary");
    ResolvedNamespace primary(ns, ns, {BSON("$match" << BSON("a" << 1))}, BSONObj());
    // Simple constructor: _userNss == ns, so userNs should be omitted on the wire.
    primary.setAdditionalResolvedNamespaces(
        {ResolvedNamespace(NamespaceString::createNamespaceString_forTest("db.coll"),
                           std::vector<BSONObj>{},
                           boost::none)});

    auto serialized = serializeToObj(primary);
    auto additionalArr = serialized["resolvedView"].Obj()["additionalResolvedNamespaces"].Array();
    ASSERT_EQ(additionalArr.size(), 1u);
    BSONObj entry = additionalArr[0].Obj();
    ASSERT_FALSE(entry.hasField("userNs"));
    ASSERT_EQ(entry["ns"].str(), "db.coll");
}

TEST(ResolvedNamespaceParseFromBSONTest, ParseFromBSONAndFromBSONRoundTripEqually) {
    const auto ns = NamespaceString::createNamespaceString_forTest("db.coll");
    ResolvedNamespace original = ResolvedNamespace::makeWithSentinelPrimary({
        ResolvedNamespace(NamespaceString::createNamespaceString_forTest("db.view"),
                          NamespaceString::createNamespaceString_forTest("db.backing"),
                          {BSON("$match" << BSON("x" << 1))},
                          BSON("locale" << "fr")),
    });

    BSONObj serialized = serializeToObj(original);

    // Additional fields must be nested inside resolvedView, not alongside it.
    ASSERT_TRUE(serialized["resolvedView"].Obj().hasField("additionalResolvedNamespaces"));
    ASSERT_TRUE(serialized["resolvedView"].Obj().hasField("isSentinelPrimary"));
    ASSERT_FALSE(serialized.hasField("additionalResolvedNamespaces"));
    ASSERT_FALSE(serialized.hasField("isSentinelPrimary"));

    ResolvedNamespace viaFromBSON = ResolvedNamespace::fromBSON(serialized);
    ResolvedNamespace viaParseFromBSON =
        ResolvedNamespace::parseFromBSON(serialized.getField("resolvedView"));

    for (const auto& parsed : {viaFromBSON, viaParseFromBSON}) {
        ASSERT_TRUE(parsed.hasSentinelPrimary());
        ASSERT_EQ(parsed.getAdditionalResolvedNamespaces().size(), 1u);
        const auto& rn = parsed.getAdditionalResolvedNamespaces()[0];
        ASSERT_EQ(rn.getNamespace().ns_forTest(), "db.view");
        ASSERT_EQ(rn.getResolvedNamespace().ns_forTest(), "db.backing");
        ASSERT_BSONOBJ_EQ(rn.getBsonPipeline()[0], BSON("$match" << BSON("x" << 1)));
        ASSERT_BSONOBJ_EQ(rn.getDefaultCollation(), BSON("locale" << "fr"));
    }
}

TEST(ResolvedNamespaceSerializeTest, BuildsSentinelPrimaryWithOnlyAdditional) {
    std::vector<ResolvedNamespace> additional = {
        ResolvedNamespace(NamespaceString::createNamespaceString_forTest("db.view1"),
                          NamespaceString::createNamespaceString_forTest("db.coll1"),
                          {BSON("$match" << BSON("x" << 1))},
                          BSONObj()),
    };
    ResolvedNamespace rn = ResolvedNamespace::makeWithSentinelPrimary(std::move(additional));
    ASSERT_TRUE(rn.hasSentinelPrimary());
    ASSERT_EQ(rn.getAdditionalResolvedNamespaces().size(), 1u);

    ResolvedNamespace parsed = ResolvedNamespace::fromBSON(serializeToObj(rn));
    ASSERT_TRUE(parsed.hasSentinelPrimary());
    ASSERT_EQ(parsed.getAdditionalResolvedNamespaces().size(), 1u);
}

}  // namespace
}  // namespace mongo
