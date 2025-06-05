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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/timeseries/bucket_spec.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/time_support.h"

#include <initializer_list>
#include <list>
#include <memory>
#include <vector>

namespace mongo {
namespace {

using InternalUnpackBucketPredicateMappingOptimizationTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsGTPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$gt: 1}}}")),
                        getExpCtx());
    const auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$or: [ {'control.max.a': {$_internalExprGt: 1}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsGTEPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$gte: 1}}}")),
                        getExpCtx());
    const auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$or: [ {'control.max.a': {$_internalExprGte: 1}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsLTPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$lt: 1}}}")),
                        getExpCtx());
    const auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$or: [ {'control.min.a': {$_internalExprLt: 1}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsLTEPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$lte: 1}}}")),
                        getExpCtx());
    const auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$or: [ {'control.min.a': {$_internalExprLte: 1}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsEQPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$eq: 1}}}")),
                        getExpCtx());
    const auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$or: [ {$and:[{'control.min.a': {$_internalExprLte: 1}},"
                               "{'control.max.a': {$_internalExprGte: 1}}]},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsINPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$in: [1, 2]}}}")),
                        getExpCtx());
    const auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate.loosePredicate);
    auto expected = fromjson(
        "{$or: ["
        "  {$or: ["
        "    {$and: ["
        "      {'control.min.a': {$_internalExprLte: 1}},"
        "      {'control.max.a': {$_internalExprGte: 1}}"
        "    ]},"
        "    {$expr: {$ne: ["
        "      {$type: [ \"$control.min.a\" ]},"
        "      {$type: [ \"$control.max.a\" ]}"
        "    ]}}"
        "  ]},"
        "  {$or: ["
        "    {$and: ["
        "      {'control.min.a': {$_internalExprLte: 2}},"
        "      {'control.max.a': {$_internalExprGte: 2}}"
        "    ]},"
        "    {$expr: {$ne: ["
        "      {$type: [ \"$control.min.a\" ]},"
        "      {$type: [ \"$control.max.a\" ]}"
        "    ]}}"
        "  ]}"
        "]}");
    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(), expected);
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsAggGTPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$expr: {$gt: [\"$a\", 1]}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    // $_internalUnpackBucket's doOptimizeAt optimizes the end of the pipeline before attempting to
    // perform predicate mapping. We will mimic this behavior here to take advantage of the
    // existing $expr rewrite optimizations.
    Pipeline::optimizeEndOfPipeline(container.begin(), &container);

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$or: [ {'control.max.a': {$_internalExprGt: 1}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsAggGTEPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$expr: {$gte: [\"$a\", 1]}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    // $_internalUnpackBucket's doOptimizeAt optimizes the end of the pipeline before attempting to
    // perform predicate mapping. We will mimic this behavior here to take advantage of the
    // existing $expr rewrite optimizations.
    Pipeline::optimizeEndOfPipeline(container.begin(), &container);

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$or: [ {'control.max.a': {$_internalExprGte: 1}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsAggLTPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$expr: {$lt: [\"$a\", 1]}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    // $_internalUnpackBucket's doOptimizeAt optimizes the end of the pipeline before attempting to
    // perform predicate mapping. We will mimic this behavior here to take advantage of the
    // existing $expr rewrite optimizations.
    Pipeline::optimizeEndOfPipeline(container.begin(), &container);

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_FALSE(predicate.loosePredicate);
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsAggLTEPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$expr: {$lte: [\"$a\", 1]}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    // $_internalUnpackBucket's doOptimizeAt optimizes the end of the pipeline before attempting to
    // perform predicate mapping. We will mimic this behavior here to take advantage of the
    // existing $expr rewrite optimizations.
    Pipeline::optimizeEndOfPipeline(container.begin(), &container);

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_FALSE(predicate.loosePredicate);
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsAggEQPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$expr: {$eq: [\"$a\", 1]}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    // $_internalUnpackBucket's doOptimizeAt optimizes the end of the pipeline before attempting to
    // perform predicate mapping. We will mimic this behavior here to take advantage of the
    // existing $expr rewrite optimizations.
    Pipeline::optimizeEndOfPipeline(container.begin(), &container);

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$or: [ {$and:[{'control.min.a': {$_internalExprLte: 1}},"
                               "{'control.max.a': {$_internalExprGte: 1}}]},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsAndWithPushableChildrenOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$and: [{b: {$gt: 1}}, {a: {$lt: 5}}]}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$and: [ {$or: [ {'control.max.b': {$_internalExprGt: 1}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.b\" ]},"
                               "{$type: [ \"$control.max.b\" ]} ]}} ]},"
                               "{$or: [ {'control.min.a': {$_internalExprLt: 5}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapAndWithUnpushableChildrenOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$and: [{b: {$ne: 1}}, {a: {$ne: 5}}]}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate.loosePredicate == nullptr);
    ASSERT(predicate.tightPredicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsAndWithPushableAndUnpushableChildrenOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$and: [{b: {$gt: 1}}, {a: {$ne: 5}}]}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$or: ["
                               "  {'control.max.b': {$_internalExprGt: 1}},"
                               "  {$expr: {$ne: ["
                               "    {$type: [ \"$control.min.b\" ]},"
                               "    {$type: [ \"$control.max.b\" ]}"
                               "  ]}}"
                               "]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsNestedAndWithPushableChildrenOnControlField) {
    auto pipeline = Pipeline::parse(
        makeVector(
            fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 'time', "
                     "bucketMaxSpanSeconds: 3600}}"),
            fromjson("{$match: {$and: [{b: {$gte: 2}}, {$and: [{b: {$gt: 1}}, {a: {$lt: 5}}]}]}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$and: [ {$or: [ {'control.max.b': {$_internalExprGte: 2}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.b\" ]},"
                               "{$type: [ \"$control.max.b\" ]} ]}} ]},"
                               "{$and: [ {$or: [ {'control.max.b': {$_internalExprGt: 1}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.b\" ]},"
                               "{$type: [ \"$control.max.b\" ]} ]}} ]},"
                               "{$or: [ {'control.min.a': {$_internalExprLt: 5}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]} ]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsOrWithPushableChildrenOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$or: [{b: {$gt: 1}}, {a: {$lt: 5}}]}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate.loosePredicate);
    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$or: ["
                               "  {$or: ["
                               "    {'control.max.b': {$_internalExprGt: 1}},"
                               "    {$expr: {$ne: ["
                               "      {$type: [ \"$control.min.b\" ]},"
                               "      {$type: [ \"$control.max.b\" ]}"
                               "    ]}}"
                               "  ]},"
                               "  {$or: ["
                               "    {'control.min.a': {$_internalExprLt: 5}},"
                               "    {$expr: {$ne: ["
                               "      {$type: [ \"$control.min.a\" ]},"
                               "      {$type: [ \"$control.max.a\" ]}"
                               "    ]}}"
                               "  ]}"
                               "]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapOrWithUnpushableChildrenOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$or: [{b: {$ne: 1}}, {a: {$ne: 5}}]}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate.loosePredicate == nullptr);
    ASSERT(predicate.tightPredicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapOrWithPushableAndUnpushableChildrenOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$or: [{b: {$gt: 1}}, {a: {$ne: 5}}]}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    // When a predicate can't be pushed down, it's the same as pushing down a trivially-true
    // predicate. So when any child of an $or can't be pushed down, we could generate something like
    // {$or: [ ... {$alwaysTrue: {}}, ... ]}, but then we might as well not push down the whole $or.
    ASSERT(predicate.loosePredicate == nullptr);
    ASSERT(predicate.tightPredicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsNestedOrWithPushableChildrenOnControlField) {
    auto pipeline = Pipeline::parse(
        makeVector(
            fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 'time', "
                     "bucketMaxSpanSeconds: 3600}}"),
            fromjson("{$match: {$or: [{b: {$gte: 2}}, {$or: [{b: {$gt: 1}}, {a: {$lt: 5}}]}]}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$or: ["
                               "  {$or: ["
                               "    {'control.max.b': {$_internalExprGte: 2}},"
                               "    {$expr: {$ne: ["
                               "      {$type: [ \"$control.min.b\" ]},"
                               "      {$type: [ \"$control.max.b\" ]}"
                               "    ]}}"
                               "  ]},"
                               "  {$or: ["
                               "    {$or: ["
                               "      {'control.max.b': {$_internalExprGt: 1}},"
                               "      {$expr: {$ne: ["
                               "        {$type: [ \"$control.min.b\" ]},"
                               "        {$type: [ \"$control.max.b\" ]}"
                               "      ]}}"
                               "    ]},"
                               "    {$or: ["
                               "      {'control.min.a': {$_internalExprLt: 5}},"
                               "      {$expr: {$ne: ["
                               "        {$type: [ \"$control.min.a\" ]},"
                               "        {$type: [ \"$control.max.a\" ]}"
                               "      ]}}"
                               "    ]}"
                               "  ]}"
                               "]}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeFurtherOptimizesNewlyAddedMatchWithSingletonAndNode) {
    auto unpackBucketObj = fromjson(
        "{$_internalUnpackBucket: {exclude: [], timeField: 'time', bucketMaxSpanSeconds: 3600}}");
    auto matchObj = fromjson("{$match: {$and: [{b: {$gt: 1}}, {a: {$ne: 5}}]}}");
    auto pipeline = Pipeline::parse(makeVector(unpackBucketObj, matchObj), getExpCtx());
    ASSERT_EQ(pipeline->size(), 2U);

    pipeline->optimizePipeline();
    ASSERT_EQ(pipeline->size(), 2U);

    // To get the optimized $match from the pipeline, we have to serialize with explain.
    auto stages = pipeline->writeExplainOps(SerializationOptions{
        .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)});
    ASSERT_EQ(stages.size(), 2U);

    ASSERT_BSONOBJ_EQ(stages[0].getDocument().toBson(),
                      fromjson("{$match: {$or: [ {'control.max.b': {$_internalExprGt: 1}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.b\" ]},"
                               "{$type: [ \"$control.max.b\" ]} ]}} ]}}"));
    ASSERT_BSONOBJ_EQ(
        stages[1].getDocument().toBson(),
        fromjson(
            "{$_internalUnpackBucket: {exclude: [], timeField: 'time', bucketMaxSpanSeconds: 3600, "
            "eventFilter: { $and: [ { b: { $gt: 1 } }, { a: { $not: { $eq: 5 } } } ] }}}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeFurtherOptimizesNewlyAddedMatchWithNestedAndNodes) {
    auto unpackBucketObj = fromjson(
        "{$_internalUnpackBucket: {exclude: [], timeField: 'time', bucketMaxSpanSeconds: 3600}}");
    auto matchObj =
        fromjson("{$match: {$and: [{b: {$gte: 2}}, {$and: [{c: {$gt: 1}}, {a: {$lt: 5}}]}]}}");
    auto pipeline = Pipeline::parse(makeVector(unpackBucketObj, matchObj), getExpCtx());
    ASSERT_EQ(pipeline->size(), 2U);

    pipeline->optimizePipeline();
    ASSERT_EQ(pipeline->size(), 2U);

    auto stages = pipeline->serializeToBson();
    ASSERT_EQ(stages.size(), 2U);

    ASSERT_BSONOBJ_EQ(
        stages[0],
        fromjson("{$match: {$and: [ {$or: [ {'control.max.b': {$_internalExprGte: 2}},"
                 "{$expr: {$ne: [ {$type: [ \"$control.min.b\" ]},"
                 "{$type: [ \"$control.max.b\" ]} ]}} ]},"
                 "{$or: [ {'control.max.c': {$_internalExprGt: 1}},"
                 "{$expr: {$ne: [ {$type: [ \"$control.min.c\" ]},"
                 "{$type: [ \"$control.max.c\" ]} ]}} ]},"
                 "{$or: [ {'control.min.a': {$_internalExprLt: 5}},"
                 "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                 "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}}"));
    ASSERT_BSONOBJ_EQ(stages[1],
                      fromjson("{ $_internalUnpackBucket: { exclude: [], timeField: \"time\", "
                               "bucketMaxSpanSeconds: 3600,"
                               "eventFilter: { $and: [ { b: { $gte: 2 } }, { c: { $gt: 1 } }, { a: "
                               "{ $lt: 5 } } ] } } }"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapPredicatesOnTypeObject) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$gt: {b: 5}}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate.loosePredicate == nullptr);
    ASSERT(predicate.tightPredicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapPredicatesOnTypeArray) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$gt: [5]}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate.loosePredicate == nullptr);
    ASSERT(predicate.tightPredicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapPredicatesOnTypeNull) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$gt: null}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate.loosePredicate == nullptr);
    ASSERT(predicate.tightPredicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapMetaPredicatesOnControlField) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {myMeta: {$gt: 5}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    // Meta predicates are mapped to the meta field, not the control min/max fields.
    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(), fromjson("{meta: {$gt: 5}}"));
    ASSERT_BSONOBJ_EQ(predicate.tightPredicate->serialize(), fromjson("{meta: {$gt: 5}}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapMetaPredicatesWithNestedFieldsOnControlField) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {'myMeta.foo': {$gt: 5}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    // Meta predicates are mapped to the meta field, not the control min/max fields.
    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(), fromjson("{'meta.foo': {$gt: 5}}"));
    ASSERT_BSONOBJ_EQ(predicate.tightPredicate->serialize(), fromjson("{'meta.foo': {$gt: 5}}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapNestedMetaPredicatesOnControlField) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {$and: [{a: {$gt: 1}}, {myMeta: {$eq: 5}}]}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$and: ["
                               "  {$or: ["
                               "    {'control.max.a': {$_internalExprGt: 1}},"
                               "    {$expr: {$ne: ["
                               "      {$type: [ \"$control.min.a\" ]},"
                               "      {$type: [ \"$control.max.a\" ]}"
                               "    ]}}"
                               "  ]},"
                               "  {meta: {$eq: 5}}"
                               "]}"));
    ASSERT(predicate.tightPredicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapPredicatesOnComputedMetaField) {
    auto pipeline = Pipeline::parse(
        makeVector(
            fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 'time', metaField: "
                     "'myMeta', computedMetaProjFields: ['myMeta'], bucketMaxSpanSeconds: 3600}}"),
            fromjson("{$project: {computedMeta: {$concat: ['Computed: ', '$myMeta']}, time: "
                     "1, meta: 1}}"),
            fromjson("{$match: {myMeta: {$eq: 'value'}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 3U);

    auto matchStage = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(matchStage->getMatchExpression());

    ASSERT_EQ(predicate.loosePredicate, nullptr);
    ASSERT_EQ(predicate.tightPredicate, nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapPredicatesOnExcludedMetaField) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: ['myMeta'], timeField: 'time', "
                            "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {myMeta: {$eq: 'value'}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_EQ(predicate.loosePredicate, nullptr);
    ASSERT_EQ(predicate.tightPredicate, nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest, OptimizeMapsAndWithOneChild) {
    // Validate that $and will get optimized out and predicates are populated correctly when we
    // have an $and with one child expression.
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                            "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {$and: ["
                            "{myMeta: {$gte: 1}}"
                            "]}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(container.size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(), fromjson("{meta: {$gte: 1}}"));
    ASSERT_BSONOBJ_EQ(predicate.tightPredicate->serialize(), fromjson("{meta: {$gte: 1}}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest, OptimizeMapsOrWithOneChild) {
    // Validate that $or will get optimized out and predicates are populated correctly when we have
    // an $or with one child expression.
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                            "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {$or: ["
                            "{myMeta: {$gte: 1}}"
                            "]}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(container.size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(), fromjson("{meta: {$gte: 1}}"));
    ASSERT_BSONOBJ_EQ(predicate.tightPredicate->serialize(), fromjson("{meta: {$gte: 1}}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest, OptimizeMapsTimePredicatesOnId) {
    auto date = Date_t::now();
    const auto dateMinusBucketSpan = date - Seconds{3600};
    const auto datePlusBucketSpan = date + Seconds{3600};
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$lt" << date)));
        auto aggTimePred =
            BSON("$match" << BSON("$expr" << BSON("$lt" << BSON_ARRAY("$time" << date))));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       aggTimePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            // $_internalUnpackBucket's doOptimizeAt optimizes the end of the pipeline before
            // attempting to perform predicate mapping. We will mimic this behavior here to take
            // advantage of the existing $expr rewrite optimizations.
            Pipeline::optimizeEndOfPipeline(container.begin(), &container);

            ASSERT_EQ(pipeline->size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto unpackStage =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get());
            auto predicate =
                unpackStage->createPredicatesOnBucketLevelField(original->getMatchExpression());
            MatchExpression* loosePredicate = predicate.loosePredicate.get();
            ASSERT_TRUE(unpackStage->generateBucketLevelIdPredicates(loosePredicate));

            auto andExpr = dynamic_cast<AndMatchExpression*>(loosePredicate);
            auto children = andExpr->getChildVector();

            ASSERT_EQ(children->size(), 3);
            ASSERT_BSONOBJ_EQ((*children)[0]->serialize(),
                              BSON("control.min.time" << BSON("$_internalExprLt" << date)));
            ASSERT_BSONOBJ_EQ(
                (*children)[1]->serialize(),
                BSON("control.max.time" << BSON("$_internalExprLt" << datePlusBucketSpan)));

            auto idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[2].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::oid);

            // As ObjectId holds time at a second granularity, the rewrite value used for a $lt/$lte
            // predicate on _id may be rounded up by a second to missing results due to trunacted
            // milliseconds.
            Date_t adjustedDate = date;
            if (adjustedDate.toMillisSinceEpoch() % 1000 != 0) {
                adjustedDate += Seconds{1};
            }

            OID oid;
            oid.init(adjustedDate);
            ASSERT_TRUE(oid.compare(idPred->getData().OID()) == 0);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$lte" << date)));
        auto aggTimePred =
            BSON("$match" << BSON("$expr" << BSON("$lte" << BSON_ARRAY("$time" << date))));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       aggTimePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            // $_internalUnpackBucket's doOptimizeAt optimizes the end of the pipeline before
            // attempting to perform predicate mapping. We will mimic this behavior here to take
            // advantage of the existing $expr rewrite optimizations.
            Pipeline::optimizeEndOfPipeline(container.begin(), &container);

            ASSERT_EQ(pipeline->size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto unpackStage =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get());
            auto predicate =
                unpackStage->createPredicatesOnBucketLevelField(original->getMatchExpression());
            MatchExpression* loosePredicate = predicate.loosePredicate.get();
            ASSERT_TRUE(unpackStage->generateBucketLevelIdPredicates(loosePredicate));

            auto andExpr = dynamic_cast<AndMatchExpression*>(loosePredicate);
            auto children = andExpr->getChildVector();

            ASSERT_EQ(children->size(), 3);
            ASSERT_BSONOBJ_EQ((*children)[0]->serialize(),
                              BSON("control.min.time" << BSON("$_internalExprLte" << date)));
            ASSERT_BSONOBJ_EQ(
                (*children)[1]->serialize(),
                BSON("control.max.time" << BSON("$_internalExprLte" << datePlusBucketSpan)));

            auto idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[2].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::oid);

            OID oid;
            oid.init(date);
            ASSERT_TRUE(oid.compare(idPred->getData().OID()) < 0);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$eq" << date)));
        auto aggTimePred =
            BSON("$match" << BSON("$expr" << BSON("$eq" << BSON_ARRAY("$time" << date))));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       aggTimePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            // $_internalUnpackBucket's doOptimizeAt optimizes the end of the pipeline before
            // attempting to perform predicate mapping. We will mimic this behavior here to take
            // advantage of the existing $expr rewrite optimizations.
            Pipeline::optimizeEndOfPipeline(container.begin(), &container);

            ASSERT_EQ(pipeline->size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto unpackStage =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get());
            auto predicate =
                unpackStage->createPredicatesOnBucketLevelField(original->getMatchExpression());
            MatchExpression* loosePredicate = predicate.loosePredicate.get();
            ASSERT_TRUE(unpackStage->generateBucketLevelIdPredicates(loosePredicate));

            auto andExpr = dynamic_cast<AndMatchExpression*>(loosePredicate);
            auto children = andExpr->getChildVector();

            ASSERT_EQ(children->size(), 6);
            ASSERT_BSONOBJ_EQ((*children)[0]->serialize(),
                              BSON("control.min.time" << BSON("$_internalExprLte" << date)));
            ASSERT_BSONOBJ_EQ(
                (*children)[1]->serialize(),
                BSON("control.min.time" << BSON("$_internalExprGte" << dateMinusBucketSpan)));
            ASSERT_BSONOBJ_EQ((*children)[2]->serialize(),
                              BSON("control.max.time" << BSON("$_internalExprGte" << date)));
            ASSERT_BSONOBJ_EQ(
                (*children)[3]->serialize(),
                BSON("control.max.time" << BSON("$_internalExprLte" << datePlusBucketSpan)));

            auto idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[4].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::oid);

            OID oid;
            oid.init(date);
            ASSERT_TRUE(oid.compare(idPred->getData().OID()) < 0);

            idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[5].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::oid);

            ASSERT_TRUE(oid.compare(idPred->getData().OID()) > 0);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$gt" << date)));
        auto aggTimePred =
            BSON("$match" << BSON("$expr" << BSON("$gt" << BSON_ARRAY("$time" << date))));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       aggTimePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            // $_internalUnpackBucket's doOptimizeAt optimizes the end of the pipeline before
            // attempting to perform predicate mapping. We will mimic this behavior here to take
            // advantage of the existing $expr rewrite optimizations.
            Pipeline::optimizeEndOfPipeline(container.begin(), &container);

            ASSERT_EQ(pipeline->size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto unpackStage =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get());
            auto predicate =
                unpackStage->createPredicatesOnBucketLevelField(original->getMatchExpression());
            MatchExpression* loosePredicate = predicate.loosePredicate.get();
            ASSERT_TRUE(unpackStage->generateBucketLevelIdPredicates(loosePredicate));

            auto andExpr = dynamic_cast<AndMatchExpression*>(loosePredicate);
            auto children = andExpr->getChildVector();

            ASSERT_EQ(children->size(), 3);
            ASSERT_BSONOBJ_EQ((*children)[0]->serialize(),
                              BSON("control.max.time" << BSON("$_internalExprGt" << date)));
            ASSERT_BSONOBJ_EQ(
                (*children)[1]->serialize(),
                BSON("control.min.time" << BSON("$_internalExprGt" << dateMinusBucketSpan)));

            auto idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[2].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::oid);

            OID oid;
            oid.init(dateMinusBucketSpan);
            ASSERT_TRUE(oid.compare(idPred->getData().OID()) < 0);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$gte" << date)));
        auto aggTimePred =
            BSON("$match" << BSON("$expr" << BSON("$gte" << BSON_ARRAY("$time" << date))));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       aggTimePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            // $_internalUnpackBucket's doOptimizeAt optimizes the end of the pipeline before
            // attempting to perform predicate mapping. We will mimic this behavior here to take
            // advantage of the existing $expr rewrite optimizations.
            Pipeline::optimizeEndOfPipeline(container.begin(), &container);

            ASSERT_EQ(pipeline->size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto unpackStage =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get());
            auto predicate =
                unpackStage->createPredicatesOnBucketLevelField(original->getMatchExpression());
            MatchExpression* loosePredicate = predicate.loosePredicate.get();
            ASSERT_TRUE(unpackStage->generateBucketLevelIdPredicates(loosePredicate));

            auto andExpr = dynamic_cast<AndMatchExpression*>(loosePredicate);
            auto children = andExpr->getChildVector();

            ASSERT_EQ(children->size(), 3);
            ASSERT_BSONOBJ_EQ((*children)[0]->serialize(),
                              BSON("control.max.time" << BSON("$_internalExprGte" << date)));
            ASSERT_BSONOBJ_EQ(
                (*children)[1]->serialize(),
                BSON("control.min.time" << BSON("$_internalExprGte" << dateMinusBucketSpan)));

            auto idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[2].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::oid);

            OID oid;
            oid.init(date - Seconds{3600});
            ASSERT_TRUE(oid.compare(idPred->getData().OID()) == 0);
        }
    }
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsTimePredicatesWithNonDateType) {
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$lt" << 1)));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            ASSERT_EQ(pipeline->size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());

            ASSERT_FALSE(predicate.loosePredicate);
            ASSERT_FALSE(predicate.tightPredicate);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$lte" << 1)));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            ASSERT_EQ(pipeline->size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());
            ASSERT_FALSE(predicate.loosePredicate);
            ASSERT_FALSE(predicate.tightPredicate);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$eq" << 1)));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            ASSERT_EQ(pipeline->size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());
            ASSERT_FALSE(predicate.loosePredicate);
            ASSERT_FALSE(predicate.tightPredicate);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$gt" << 1)));
        auto pipelines = {

            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            ASSERT_EQ(pipeline->size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());

            ASSERT_FALSE(predicate.loosePredicate);
            ASSERT_FALSE(predicate.tightPredicate);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$gte" << 1)));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            ASSERT_EQ(pipeline->size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());
            ASSERT_FALSE(predicate.loosePredicate);
            ASSERT_FALSE(predicate.tightPredicate);
        }
    }
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsGeoWithinPredicatesUsingInternalBucketGeoWithin) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                            "'time', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {loc: {$geoWithin: {$geometry: {type: \"Polygon\", "
                            "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]}}}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(container.size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate.loosePredicate->serialize(),
                      fromjson("{$_internalBucketGeoWithin: { withinRegion: { $geometry: { type : "
                               "\"Polygon\" ,coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 "
                               "] ] ]}},field: \"loc\"}}"));
    ASSERT_FALSE(predicate.tightPredicate);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeRemoveEventFilterOnTimePredicateWithFixedBuckets) {

    auto date = Date_t::now();
    auto roundedTime = timeseries::roundTimestampBySeconds(date, 3600);

    // Validate 'rewriteProvidesExactMatchPredicate' is true when the predicate aligns with the
    // bucket boundary.
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$gte" << roundedTime)));
        auto pipeline =
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600, "
                                                "fixedBuckets: true }}"),
                                       timePred),
                            getExpCtx());
        auto& container = pipeline->getSources();

        ASSERT_EQ(container.size(), 2U);

        auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
        auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                             ->createPredicatesOnBucketLevelField(original->getMatchExpression());
        ASSERT_TRUE(predicate.rewriteProvidesExactMatchPredicate);
    }

    // Validate 'rewriteProvidesExactMatchPredicate' is false when the predicate does not align with
    // the bucket boundary.
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$gte" << date)));
        auto pipeline =
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600, "
                                                "fixedBuckets: true }}"),
                                       timePred),
                            getExpCtx());
        auto& container = pipeline->getSources();

        ASSERT_EQ(container.size(), 2U);

        auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
        auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                             ->createPredicatesOnBucketLevelField(original->getMatchExpression());
        ASSERT_FALSE(predicate.rewriteProvidesExactMatchPredicate);
    }

    // Validate 'rewriteProvidesExactMatchPredicate' is false when the predicate is before 1970.
    {
        auto minDate = Date_t::min() + Days(100);  // date before 1970.
        auto timePred = BSON("$match" << BSON("time" << BSON("$gte" << minDate)));
        auto pipeline =
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600, "
                                                "fixedBuckets: true }}"),
                                       timePred),
                            getExpCtx());
        auto& container = pipeline->getSources();

        ASSERT_EQ(container.size(), 2U);

        auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
        auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                             ->createPredicatesOnBucketLevelField(original->getMatchExpression());
        ASSERT_FALSE(predicate.rewriteProvidesExactMatchPredicate);
    }
    // Validate 'rewriteProvidesExactMatchPredicate' is false when the buckets are not fixed, even
    // if the bucket boundaries align.
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$gte" << roundedTime)));
        auto pipeline =
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600, "
                                                "fixedBuckets: false }}"),
                                       timePred),
                            getExpCtx());
        auto& container = pipeline->getSources();

        ASSERT_EQ(container.size(), 2U);

        auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
        auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                             ->createPredicatesOnBucketLevelField(original->getMatchExpression());
        ASSERT_FALSE(predicate.rewriteProvidesExactMatchPredicate);
    }
}

}  // namespace
}  // namespace mongo
