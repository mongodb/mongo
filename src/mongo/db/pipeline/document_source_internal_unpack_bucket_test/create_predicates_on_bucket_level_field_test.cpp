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
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate->serialize(true),
                      fromjson("{$or: [ {'control.max.a': {$_internalExprGt: 1}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsGTEPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$gte: 1}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate->serialize(true),
                      fromjson("{$or: [ {'control.max.a': {$_internalExprGte: 1}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsLTPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$lt: 1}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate->serialize(true),
                      fromjson("{$or: [ {'control.min.a': {$_internalExprLt: 1}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsLTEPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$lte: 1}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate->serialize(true),
                      fromjson("{$or: [ {'control.min.a': {$_internalExprLte: 1}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsEQPredicatesOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$eq: 1}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate->serialize(true),
                      fromjson("{$or: [ {$and:[{'control.min.a': {$_internalExprLte: 1}},"
                               "{'control.max.a': {$_internalExprGte: 1}}]},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsAndWithPushableChildrenOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$and: [{b: {$gt: 1}}, {a: {$lt: 5}}]}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate->serialize(true),
                      fromjson("{$and: [ {$or: [ {'control.max.b': {$_internalExprGt: 1}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.b\" ]},"
                               "{$type: [ \"$control.max.b\" ]} ]}} ]} ]},"
                               "{$or: [ {'control.min.a': {$_internalExprLt: 5}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]} ]}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapAndWithUnpushableChildrenOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$and: [{b: {$ne: 1}}, {a: {$ne: 5}}]}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeMapsAndWithPushableAndUnpushableChildrenOnControlField) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {$and: [{b: {$gt: 1}}, {a: {$ne: 5}}]}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate->serialize(true),
                      fromjson("{$and: [ {$or: [ {'control.max.b': {$_internalExprGt: 1}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.b\" ]},"
                               "{$type: [ \"$control.max.b\" ]} ]}} ]} ]} ]}"));
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

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate->serialize(true),
                      fromjson("{$and: [ {$or: [ {'control.max.b': {$_internalExprGte: 2}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.b\" ]},"
                               "{$type: [ \"$control.max.b\" ]} ]}} ]} ]},"
                               "{$and: [ {$or: [ {'control.max.b': {$_internalExprGt: 1}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.b\" ]},"
                               "{$type: [ \"$control.max.b\" ]} ]}} ]} ]},"
                               "{$or: [ {'control.min.a': {$_internalExprLt: 5}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]} ]} ]}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeFurtherOptimizesNewlyAddedMatchWithSingletonAndNode) {
    auto unpackBucketObj = fromjson(
        "{$_internalUnpackBucket: {exclude: [], timeField: 'time', bucketMaxSpanSeconds: 3600}}");
    auto matchObj = fromjson("{$match: {$and: [{b: {$gt: 1}}, {a: {$ne: 5}}]}}");
    auto pipeline = Pipeline::parse(makeVector(unpackBucketObj, matchObj), getExpCtx());
    ASSERT_EQ(pipeline->getSources().size(), 2U);

    pipeline->optimizePipeline();
    ASSERT_EQ(pipeline->getSources().size(), 3U);

    // To get the optimized $match from the pipeline, we have to serialize with explain.
    auto stages = pipeline->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(stages.size(), 3U);

    ASSERT_BSONOBJ_EQ(stages[0].getDocument().toBson(),
                      fromjson("{$match: {$or: [ {'control.max.b': {$_internalExprGt: 1}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.b\" ]},"
                               "{$type: [ \"$control.max.b\" ]} ]}} ]}}"));
    ASSERT_BSONOBJ_EQ(stages[1].getDocument().toBson(), unpackBucketObj);
    ASSERT_BSONOBJ_EQ(stages[2].getDocument().toBson(),
                      fromjson("{$match: {$and: [{b: {$gt: 1}}, {a: {$not: {$eq: 5}}}]}}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeFurtherOptimizesNewlyAddedMatchWithNestedAndNodes) {
    auto unpackBucketObj = fromjson(
        "{$_internalUnpackBucket: {exclude: [], timeField: 'time', bucketMaxSpanSeconds: 3600}}");
    auto matchObj =
        fromjson("{$match: {$and: [{b: {$gte: 2}}, {$and: [{c: {$gt: 1}}, {a: {$lt: 5}}]}]}}");
    auto pipeline = Pipeline::parse(makeVector(unpackBucketObj, matchObj), getExpCtx());
    ASSERT_EQ(pipeline->getSources().size(), 2U);

    pipeline->optimizePipeline();
    ASSERT_EQ(pipeline->getSources().size(), 3U);

    auto stages = pipeline->serializeToBson();
    ASSERT_EQ(stages.size(), 3U);

    ASSERT_BSONOBJ_EQ(
        stages[0],
        fromjson("{$match: {$and: [ {$or: [ {'control.max.b': {$_internalExprGte: 2}},"
                 "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.b\" ]},"
                 "{$type: [ \"$control.max.b\" ]} ]}} ]} ]},"
                 "{$or: [ {'control.max.c': {$_internalExprGt: 1}},"
                 "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.c\" ]},"
                 "{$type: [ \"$control.max.c\" ]} ]}} ]} ]},"
                 "{$or: [ {'control.min.a': {$_internalExprLt: 5}},"
                 "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                 "{$type: [ \"$control.max.a\" ]} ]}} ]} ]} ]}}"));
    ASSERT_BSONOBJ_EQ(stages[1], unpackBucketObj);
    ASSERT_BSONOBJ_EQ(stages[2], matchObj);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapPredicatesOnTypeObject) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$gt: {b: 5}}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapPredicatesOnTypeArray) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$gt: [5]}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapPredicatesOnTypeNull) {
    auto pipeline =
        Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                            "'time', bucketMaxSpanSeconds: 3600}}"),
                                   fromjson("{$match: {a: {$gt: null}}}")),
                        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapMetaPredicatesOnControlField) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {myMeta: {$gt: 5}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapMetaPredicatesWithNestedFieldsOnControlField) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {'myMeta.foo': {$gt: 5}}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT(predicate == nullptr);
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest,
       OptimizeDoesNotMapNestedMetaPredicatesOnControlField) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {$and: [{a: {$gt: 1}}, {myMeta: {$eq: 5}}]}}")),
        getExpCtx());
    auto& container = pipeline->getSources();

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
    auto predicate = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                         ->createPredicatesOnBucketLevelField(original->getMatchExpression());

    ASSERT_BSONOBJ_EQ(predicate->serialize(true),
                      fromjson("{$and: [ {$or: [ {'control.max.a': {$_internalExprGt: 1}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]} ]}"));
}

TEST_F(InternalUnpackBucketPredicateMappingOptimizationTest, OptimizeMapsTimePredicatesOnId) {
    auto date = Date_t::now();
    const auto dateMinusBucketSpan = date - Seconds{3600};
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$lt" << date)));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            ASSERT_EQ(pipeline->getSources().size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());

            auto andExpr = dynamic_cast<AndMatchExpression*>(predicate.get());
            auto children = andExpr->getChildVector();

            ASSERT_EQ(children->size(), 2);
            ASSERT_BSONOBJ_EQ((*children)[0]->serialize(true),
                              BSON("control.min.time" << BSON("$_internalExprLt" << date)));

            auto idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[1].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::jstOID);

            OID oid;
            oid.init(date);
            ASSERT_TRUE(oid.compare(idPred->getData().OID()) == 0);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$lte" << date)));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            ASSERT_EQ(pipeline->getSources().size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());
            auto andExpr = dynamic_cast<AndMatchExpression*>(predicate.get());
            auto children = andExpr->getChildVector();

            ASSERT_EQ(children->size(), 2);
            ASSERT_BSONOBJ_EQ((*children)[0]->serialize(true),
                              BSON("control.min.time" << BSON("$_internalExprLte" << date)));

            auto idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[1].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::jstOID);

            OID oid;
            oid.init(date);
            ASSERT_TRUE(oid.compare(idPred->getData().OID()) < 0);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$eq" << date)));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            ASSERT_EQ(pipeline->getSources().size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());
            auto andExpr = dynamic_cast<AndMatchExpression*>(predicate.get());
            auto children = andExpr->getChildVector();

            ASSERT_EQ(children->size(), 5);
            ASSERT_BSONOBJ_EQ((*children)[0]->serialize(true),
                              BSON("control.min.time" << BSON("$_internalExprLte" << date)));
            ASSERT_BSONOBJ_EQ(
                (*children)[1]->serialize(true),
                BSON("control.min.time" << BSON("$_internalExprGte" << dateMinusBucketSpan)));
            ASSERT_BSONOBJ_EQ((*children)[2]->serialize(true),
                              BSON("control.max.time" << BSON("$_internalExprGte" << date)));

            auto idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[3].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::jstOID);

            OID oid;
            oid.init(date);
            ASSERT_TRUE(oid.compare(idPred->getData().OID()) < 0);

            idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[4].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::jstOID);

            ASSERT_TRUE(oid.compare(idPred->getData().OID()) > 0);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$gt" << date)));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            ASSERT_EQ(pipeline->getSources().size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());

            auto andExpr = dynamic_cast<AndMatchExpression*>(predicate.get());
            auto children = andExpr->getChildVector();

            ASSERT_EQ(children->size(), 3);
            ASSERT_BSONOBJ_EQ((*children)[0]->serialize(true),
                              BSON("control.max.time" << BSON("$_internalExprGt" << date)));
            ASSERT_BSONOBJ_EQ(
                (*children)[1]->serialize(true),
                BSON("control.min.time" << BSON("$_internalExprGt" << dateMinusBucketSpan)));

            auto idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[2].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::jstOID);

            OID oid;
            oid.init(dateMinusBucketSpan);
            ASSERT_TRUE(oid.compare(idPred->getData().OID()) < 0);
        }
    }
    {
        auto timePred = BSON("$match" << BSON("time" << BSON("$gte" << date)));
        auto pipelines = {
            Pipeline::parse(makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                                                "'time', bucketMaxSpanSeconds: 3600}}"),
                                       timePred),
                            getExpCtx()),
            Pipeline::parse(makeVector(fromjson("{$_unpackBucket: {timeField: 'time'}}"), timePred),
                            getExpCtx())};
        for (auto& pipeline : pipelines) {
            auto& container = pipeline->getSources();

            ASSERT_EQ(pipeline->getSources().size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());
            auto andExpr = dynamic_cast<AndMatchExpression*>(predicate.get());
            auto children = andExpr->getChildVector();

            ASSERT_EQ(children->size(), 3);
            ASSERT_BSONOBJ_EQ((*children)[0]->serialize(true),
                              BSON("control.max.time" << BSON("$_internalExprGte" << date)));
            ASSERT_BSONOBJ_EQ(
                (*children)[1]->serialize(true),
                BSON("control.min.time" << BSON("$_internalExprGte" << dateMinusBucketSpan)));

            auto idPred = dynamic_cast<ComparisonMatchExpressionBase*>((*children)[2].get());

            ASSERT_EQ(idPred->path(), "_id"_sd);
            ASSERT_EQ(idPred->getData().type(), BSONType::jstOID);

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

            ASSERT_EQ(pipeline->getSources().size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());

            ASSERT_FALSE(predicate);
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

            ASSERT_EQ(pipeline->getSources().size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());
            ASSERT_FALSE(predicate);
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

            ASSERT_EQ(pipeline->getSources().size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());
            ASSERT_FALSE(predicate);
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

            ASSERT_EQ(pipeline->getSources().size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());

            ASSERT_FALSE(predicate);
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

            ASSERT_EQ(pipeline->getSources().size(), 2U);

            auto original = dynamic_cast<DocumentSourceMatch*>(container.back().get());
            auto predicate =
                dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                    ->createPredicatesOnBucketLevelField(original->getMatchExpression());
            ASSERT_FALSE(predicate);
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

    ASSERT_BSONOBJ_EQ(predicate->serialize(true),
                      fromjson("{$_internalBucketGeoWithin: { withinRegion: { $geometry: { type : "
                               "\"Polygon\" ,coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 "
                               "] ] ]}},field: \"loc\"}}"));
}
}  // namespace
}  // namespace mongo
