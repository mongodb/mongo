/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/bson_test_util.h"

namespace mongo {
namespace {

using InternalUnpackBucketSplitMatchOnMetaAndRename = AggregationContextFixture;

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, OptimizeSplitsMatchAndMapsControlPredicates) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(
        makeVector(unpack, fromjson("{$match: {myMeta: {$gte: 0, $lte: 5}, a: {$lte: 4}}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should split and rename the $match. A separate optimization maps the predicate on 'a' to a
    // predicate on 'control.min.a'. These two created $match stages should be added before
    // $_internalUnpackBucket and merged.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{meta: {$gte: 0}}, {meta: {$lte: 5}}, "
                               "{$or: [ {'control.min.a': {$_internalExprLte: 4}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]} ]}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(unpack, serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {a: {$lte: 4}}}"), serialized[2]);
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, OptimizeMovesMetaMatchBeforeUnpack) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline =
        Pipeline::parse(makeVector(unpack, fromjson("{$match: {myMeta: {$gte: 0}}}")), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // The $match on meta is moved before $_internalUnpackBucket and no other optimization is done.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$gte: 0}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(unpack, serialized[1]);
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename,
       OptimizeDoesNotMoveMetaMatchBeforeUnpackWithExclusionOnMeta) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto unpackExcluded = fromjson(
        "{$_internalUnpackBucket: { include: ['_id', 'data'], timeField: 'foo', metaField: "
        "'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(makeVector(unpack,
                                               fromjson("{$project: {data: 1}}"),
                                               fromjson("{$match: {myMeta: {$gte: 0}}}")),
                                    getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // The $match on meta is not moved before $_internalUnpackBucket since the field is excluded.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackExcluded, serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {myMeta: {$gte: 0}}}"), serialized[1]);
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename,
       OptimizeDoesNotErrorOnFailedSplitOfMetaMatch) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto match = fromjson(
        "{$match: {$and: [{x: {$lte: 1}}, {$or: [{'myMeta.a': "
        "{$gt: 1}}, {y: {$lt: 1}}]}]}}");
    auto pipeline = Pipeline::parse(makeVector(unpack, match), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should fail to split the match because of the $or clause. We should still be able to
    // map the predicate on 'x' to a predicate on the control field.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{$or: [ {'control.min.x': {$_internalExprLte: 1}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.x\" ]},"
                               "{$type: [ \"$control.max.x\" ]} ]}} ]} ]} ]}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(unpack, serialized[1]);
    ASSERT_BSONOBJ_EQ(match, serialized[2]);
}
}  // namespace
}  // namespace mongo
