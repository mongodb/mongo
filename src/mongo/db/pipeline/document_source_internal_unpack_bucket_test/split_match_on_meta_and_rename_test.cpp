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

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, DoesNotSplitWhenNoMetaFieldSpecified) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', bucketMaxSpanSeconds: "
                 "3600}}")
            .firstElement(),
        getExpCtx());
    auto matchToSplit = DocumentSourceMatch::create(fromjson("{meta: {$gt: 1}}"), getExpCtx());

    auto [metaOnlyMatch, remainingMatch] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->splitMatchOnMetaAndRename(matchToSplit.get());

    // Can't split when there is no metaField specified in the stage.
    ASSERT_FALSE(metaOnlyMatch);
    ASSERT_TRUE(remainingMatch);
    ASSERT_BSONOBJ_EQ(matchToSplit->getQuery(), remainingMatch->getQuery());
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, DoesNotSplitWhenNoMatchOnMetaField) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto matchToSplit = DocumentSourceMatch::create(fromjson("{a: {$gt: 1}}"), getExpCtx());

    auto [metaOnlyMatch, remainingMatch] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->splitMatchOnMetaAndRename(matchToSplit.get());

    // Can't split when the match does not reference the metaField.
    ASSERT_FALSE(metaOnlyMatch);
    ASSERT_TRUE(remainingMatch);
    ASSERT_BSONOBJ_EQ(matchToSplit->getQuery(), remainingMatch->getQuery());
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, SplitsWhenEntireMatchIsOnMetaField) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto matchToSplit = DocumentSourceMatch::create(
        fromjson("{$or: [{myMeta: {$gt: 1}}, {'myMeta.a': {$lt: 1}}]}"), getExpCtx());

    auto [metaOnlyMatch, remainingMatch] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->splitMatchOnMetaAndRename(matchToSplit.get());

    // Can split and rename when the match is entirely on the metaField.
    ASSERT_TRUE(metaOnlyMatch);
    ASSERT_BSONOBJ_EQ(fromjson("{$or: [{meta: {$gt: 1}}, {'meta.a': {$lt: 1}}]}"),
                      metaOnlyMatch->getQuery());
    ASSERT_FALSE(remainingMatch);
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename,
       SplitsWhenIndependentPartOfMatchIsOnMetaField) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto matchToSplit = DocumentSourceMatch::create(
        fromjson("{$and: [{'myMeta.a': {$gt: 1}}, {b: {$lt: 1}}]}"), getExpCtx());

    auto [metaOnlyMatch, remainingMatch] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->splitMatchOnMetaAndRename(matchToSplit.get());

    // Can split and rename when an independent part of the match is on the metaField.
    ASSERT_TRUE(metaOnlyMatch);
    ASSERT_BSONOBJ_EQ(fromjson("{'meta.a': {$gt: 1}}"), metaOnlyMatch->getQuery());
    ASSERT_TRUE(remainingMatch);
    ASSERT_BSONOBJ_EQ(fromjson("{b: {$lt: 1}}"), remainingMatch->getQuery());
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename,
       DoesNotSplitsWhenDependentPartOfMatchIsOnMetaField) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto matchToSplit = DocumentSourceMatch::create(
        fromjson("{$or: [{'meta.a': {$gt: 1}}, {metaXYZ: {$lt: 1}}]}"), getExpCtx());

    auto [metaOnlyMatch, remainingMatch] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->splitMatchOnMetaAndRename(matchToSplit.get());

    // Can't split when the part of the match that is on the metaField is dependent on the rest.
    // Even though 'metaXYZ' is prefixed by 'meta', it's not a subfield. The presence of a top-level
    // $or means this match cannot be correctly split into two matches.
    ASSERT_FALSE(metaOnlyMatch);
    ASSERT_TRUE(remainingMatch);
    ASSERT_BSONOBJ_EQ(matchToSplit->getQuery(), remainingMatch->getQuery());
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, SplitsWhenSharedPrefixOfMetaIsNotSubfield) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto matchToSplit = DocumentSourceMatch::create(
        fromjson("{$and: [{myMeta: {$gt: 1}}, {myMetaXYZ: {$lt: 1}}]}"), getExpCtx());

    auto [metaOnlyMatch, remainingMatch] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->splitMatchOnMetaAndRename(matchToSplit.get());

    // Can split and rename when an independent part of the match is on the metaField. Even though
    // 'myMetaXYZ' is prefixed by 'myMeta', it's not a subfield, so it should not be pushed down.
    ASSERT_TRUE(metaOnlyMatch);
    ASSERT_BSONOBJ_EQ(fromjson("{meta: {$gt: 1}}"), metaOnlyMatch->getQuery());
    ASSERT_TRUE(remainingMatch);
    ASSERT_BSONOBJ_EQ(fromjson("{myMetaXYZ: {$lt: 1}}"), remainingMatch->getQuery());
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, SplitsAndRenamesWithExpr) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto matchToSplit =
        DocumentSourceMatch::create(fromjson("{$expr: {$eq: ['$myMeta.a', 2]}}"), getExpCtx());

    auto [metaOnlyMatch, remainingMatch] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->splitMatchOnMetaAndRename(matchToSplit.get());

    // Can split and rename when the $match includes a $expr.
    ASSERT_TRUE(metaOnlyMatch);
    ASSERT_BSONOBJ_EQ(fromjson("{$expr: {$eq: ['$meta.a', {$const: 2}]}}"),
                      metaOnlyMatch->getQuery());
    ASSERT_FALSE(remainingMatch);
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, SplitsAndRenamesWithType) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto matchToSplit =
        DocumentSourceMatch::create(fromjson("{myMeta: {$type: [4]}}"), getExpCtx());

    auto [metaOnlyMatch, remainingMatch] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->splitMatchOnMetaAndRename(matchToSplit.get());

    // Can split and rename when the $match includes a $type.
    ASSERT_TRUE(metaOnlyMatch);
    ASSERT_BSONOBJ_EQ(fromjson("{meta: {$type: [4]}}"), metaOnlyMatch->getQuery());
    ASSERT_FALSE(remainingMatch);
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, SplitsAndRenamesWhenMultiplePredicates) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto matchToSplit = DocumentSourceMatch::create(
        fromjson("{myMeta: {$gte: 0, $lte: 5}, l: {$type: [4]}}"), getExpCtx());

    auto [metaOnlyMatch, remainingMatch] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->splitMatchOnMetaAndRename(matchToSplit.get());

    // Can split and rename when the $match includes multiple predicates.
    ASSERT_TRUE(metaOnlyMatch);
    ASSERT_BSONOBJ_EQ(fromjson("{$and: [{meta: {$gte: 0}}, {meta: {$lte: 5}}]}"),
                      metaOnlyMatch->getQuery());
    ASSERT_TRUE(remainingMatch);
    ASSERT_BSONOBJ_EQ(fromjson("{l: {$type: [4]}}"), remainingMatch->getQuery());
}

TEST_F(InternalUnpackBucketSplitMatchOnMetaAndRename, SplitsAndRenamesWhenSeveralFieldReferences) {
    auto unpack = DocumentSourceInternalUnpackBucket::createFromBsonInternal(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}")
            .firstElement(),
        getExpCtx());
    auto matchToSplit = DocumentSourceMatch::create(
        fromjson("{$and: [{myMeta: {$type: [3]}}, {'myMeta.a': {$gte: "
                 "0}}, {'myMeta.b': {$type: [4]}}, {a: {$in: ['$b', '$c']}}]}"),
        getExpCtx());

    auto [metaOnlyMatch, remainingMatch] =
        dynamic_cast<DocumentSourceInternalUnpackBucket*>(unpack.get())
            ->splitMatchOnMetaAndRename(matchToSplit.get());

    // Can split and rename when the $match includes several field references.
    ASSERT_TRUE(metaOnlyMatch);
    ASSERT_BSONOBJ_EQ(fromjson("{$and: [{meta: {$type: [3]}}, {'meta.a': {$gte: 0}}, "
                               "{'meta.b': {$type: [4]}}]}"),
                      metaOnlyMatch->getQuery());
    ASSERT_TRUE(remainingMatch);
    ASSERT_BSONOBJ_EQ(fromjson("{a: {$in: ['$b', '$c']}}"), remainingMatch->getQuery());
}

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
