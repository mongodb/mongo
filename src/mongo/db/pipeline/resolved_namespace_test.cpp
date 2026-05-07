/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/pipeline/resolved_namespace.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

TimeseriesOptions makeTimeseriesOptions(StringData timeField) {
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

}  // namespace
}  // namespace mongo
