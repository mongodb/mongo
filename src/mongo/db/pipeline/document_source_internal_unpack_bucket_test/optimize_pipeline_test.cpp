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

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/bson_test_util.h"

namespace mongo {
namespace {

using OptimizePipeline = AggregationContextFixture;

TEST_F(OptimizePipeline, MixedMatchPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(
        makeVector(unpack, fromjson("{$match: {myMeta: {$gte: 0, $lte: 5}, a: {$lte: 4}}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // To get the optimized $match from the pipeline, we have to serialize with explain.
    auto stages = pipeline->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(3u, stages.size());

    // We should push down the $match on the metaField and the predicates on the control field.
    // The created $match stages should be added before $_internalUnpackBucket and merged.
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{meta: {$gte: 0}}, {meta: {$lte: 5}},"
                               "{$or: [ {'control.min.a': {$_internalExprLte: 4}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ] } ] } } ] }]}}"),
                      stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(unpack, stages[1].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {a: {$lte: 4}}}"), stages[2].getDocument().toBson());
}

TEST_F(OptimizePipeline, MetaMatchPushedDown) {
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

TEST_F(OptimizePipeline, MixedMatchOnlyControlPredicatesPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto match = fromjson(
        "{$match: {$and: [{x: {$lte: 1}}, {$or: [{'myMeta.a': "
        "{$gt: 1}}, {y: {$lt: 1}}]}]}}");
    auto pipeline = Pipeline::parse(makeVector(unpack, match), getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should fail to push down the $match on meta because of the $or clause. We should still be
    // able to map the predicate on 'x' to a predicate on the control field.
    auto stages = pipeline->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(3u, stages.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$or: [ {'control.min.x': {$_internalExprLte: 1}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.x\" ]},"
                               "{$type: [ \"$control.max.x\" ] } ] } } ] }}"),
                      stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(unpack, stages[1].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(match, stages[2].getDocument().toBson());
}

TEST_F(OptimizePipeline, MixedMatchOnlyMetaMatchPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(
        makeVector(unpack, fromjson("{$match: {myMeta: {$gte: 0, $lte: 5}, a: {$in: [1, 2, 3]}}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down the $match on the metaField but not the predicate on '$a', which is
    // ineligible because of the $in.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{meta: {$gte: 0}}, {meta: {$lte: 5}}]}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(unpack, serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {a: {$in: [1, 2, 3]}}}"), serialized[2]);
}

TEST_F(OptimizePipeline, MultipleMatchesPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(makeVector(unpack,
                                               fromjson("{$match: {myMeta: {$gte: 0, $lte: 5}}}"),
                                               fromjson("{$match: {a: {$lte: 4}}}")),
                                    getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down both the $match on the metaField and the predicates on the control field.
    // The created $match stages should be added before $_internalUnpackBucket and merged.
    auto stages = pipeline->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(3u, stages.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [ {meta: {$gte: 0}},"
                               "{meta: {$lte: 5}},"
                               "{$or: [ {'control.min.a': {$_internalExprLte: 4}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}}"),
                      stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(unpack, stages[1].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {a: {$lte: 4}}}"), stages[2].getDocument().toBson());
}

TEST_F(OptimizePipeline, MultipleMatchesPushedDownWithSort) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(makeVector(unpack,
                                               fromjson("{$match: {myMeta: {$gte: 0, $lte: 5}}}"),
                                               fromjson("{$sort: {a: 1}}"),
                                               fromjson("{$match: {a: {$lte: 4}}}")),
                                    getExpCtx());
    ASSERT_EQ(4u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down both the $match on the metaField and the predicates on the control field.
    // The created $match stages should be added before $_internalUnpackBucket and merged.
    auto stages = pipeline->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(4u, stages.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [ { meta: { $gte: 0 } },"
                               "{meta: { $lte: 5 } },"
                               "{$or: [ { 'control.min.a': { $_internalExprLte: 4 } },"
                               "{$expr: { $ne: [ {$type: [ \"$control.min.a\" ] },"
                               "{$type: [ \"$control.max.a\" ] } ] } } ] }]}}"),
                      stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(unpack, stages[1].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {a: {$lte: 4}}}"), stages[2].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {sortKey: {a: 1}}}"), stages[3].getDocument().toBson());
}

TEST_F(OptimizePipeline, MetaMatchThenCountPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {myMeta: {$eq: 'abc'}}}"),
                   fromjson("{$count: 'foo'}")),
        getExpCtx());
    ASSERT_EQ(4u, pipeline->getSources().size());  // $count is expanded into $group and $project.

    pipeline->optimizePipeline();

    // We should push down the $match and internalize the empty dependency set.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(4u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$eq: 'abc'}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: [], timeField: 'time', "
                               "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                      serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$group: {_id: {$const: null}, foo: {$sum: {$const: 1}}}}"),
                      serialized[2]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {foo: true, _id: false}}"), serialized[3]);
}

TEST_F(OptimizePipeline, SortThenMetaMatchPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(makeVector(unpack,
                                               fromjson("{$sort: {myMeta: -1}}"),
                                               fromjson("{$match: {myMeta: {$eq: 'abc'}}}")),
                                    getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down both the $sort and the $match.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$eq: 'abc'}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {meta: -1}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(unpack, serialized[2]);
}

TEST_F(OptimizePipeline, SortThenMixedMatchPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline =
        Pipeline::parse(makeVector(unpack,
                                   fromjson("{$sort: {myMeta: -1}}"),
                                   fromjson("{$match: {a: {$gte: 5}, myMeta: {$eq: 'abc'}}}")),
                        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down both the $sort and parts of the $match.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(4u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{meta: {$eq: 'abc'}},"
                               "{$or: [ {'control.max.a': {$_internalExprGte: 5}},"
                               "{$or: [ {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]} ]}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {meta: -1}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(unpack, serialized[2]);
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {a: {$gte: 5}}}"), serialized[3]);
}

TEST_F(OptimizePipeline, MetaMatchThenSortPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(makeVector(unpack,
                                               fromjson("{$match: {myMeta: {$eq: 'abc'}}}"),
                                               fromjson("{$sort: {myMeta: -1}}")),
                                    getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down both the $sort and the entire $match.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$eq: 'abc'}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {meta: -1}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(unpack, serialized[2]);
}

TEST_F(OptimizePipeline, MetaMatchThenProjectPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {myMeta: {$eq: 'abc'}}}"),
                   fromjson("{$project: {x: 0}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down the $match and internalize the $project.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$eq: 'abc'}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: ['x'], timeField: 'time', "
                               "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                      serialized[1]);
}

TEST_F(OptimizePipeline, MixedMatchThenProjectPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {myMeta: {$eq: 'abc'}, a: {$lte: 4}}}"),
                   fromjson("{$project: {x: 1}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We can push down part of the $match and use dependency analysis on the end of the pipeline.
    auto stages = pipeline->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(4u, stages.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{meta: {$eq: 'abc'}},"
                               "{$or: [ {'control.min.a': { $_internalExprLte: 4 } },"
                               "{$expr: { $ne: [ {$type: [ \"$control.min.a\" ] },"
                               "{$type: [ \"$control.max.a\" ] } ] } } ] } ]}}"),
                      stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['_id', 'a', 'x'], timeField: "
                               "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                      stages[1].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {a: {$lte: 4}}}"), stages[2].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {_id: true, x: true}}"),
                      stages[3].getDocument().toBson());
}


TEST_F(OptimizePipeline, ProjectThenMetaMatchPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {x: 0}}"),
                   fromjson("{$match: {myMeta: {$eq: 'abc'}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down the $match and internalize the $project.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$eq: 'abc'}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: ['x'], timeField: 'time', "
                               "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                      serialized[1]);
}

TEST_F(OptimizePipeline, ProjectThenMixedMatchPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {a: 1, myMeta: 1, x: 1}}"),
                   fromjson("{$match: {myMeta: {$eq: 'abc'}, a: {$lte: 4}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down part of the $match and do dependency analysis on the rest.
    auto stages = pipeline->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(4u, stages.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{meta: {$eq: \"abc\"}},"
                               "{$or: [ {'control.min.a': {$_internalExprLte: 4}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ] },"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}}"),
                      stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'a', 'x', 'myMeta'], timeField: "
                 "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
        stages[1].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {a: {$lte: 4}}}"), stages[2].getDocument().toBson());
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(
        kComparator.compare(fromjson("{$project: {_id: true, a: true, myMeta: true, x: true}}"),
                            stages[3].getDocument().toBson()),
        0);
}

TEST_F(OptimizePipeline, ProjectWithRenameThenMixedMatchPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {myMeta: '$y', a: 1}}"),
                   fromjson("{$match: {myMeta: {$gte: 'abc'}, a: {$lte: 4}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down part of the $match and do dependency analysis on the end of the pipeline.
    auto stages = pipeline->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(4u, stages.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$match: {$and: [{$or: [ {'control.max.y': {$_internalExprGte: \"abc\"}},"
                 "{$expr: {$ne: [ {$type: [ \"$control.min.y\" ]},"
                 "{$type: [ \"$control.max.y\" ]} ]}} ]},"
                 "{$or: [ {'control.min.a': {$_internalExprLte: 4}},"
                 "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ] },"
                 "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}}"),
        stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['_id', 'a', 'y'], timeField: "
                               "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                      stages[1].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{y: {$gte: 'abc'}}, {a: {$lte: 4}}]}}"),
                      stages[2].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {_id: true, a: true, myMeta: '$y'}}"),
                      stages[3].getDocument().toBson());
}

TEST_F(OptimizePipeline, ComputedProjectThenMetaMatchPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {y: '$myMeta'}}"),
                   fromjson("{$match: {y: {$gte: 'abc'}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down both the match and the project and internalize the remaining project.
    // Note that the $match substitutes 'y' with 'myMeta', allowing it to be moved before the
    // project and enabling pushdown.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$gte: 'abc'}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {y: '$meta'}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'y'], timeField: 'time', metaField: "
                 "'myMeta', bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['y']}}"),
        serialized[2]);
}

TEST_F(OptimizePipeline, ComputedProjectThenMetaMatchNotPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {myMeta: {$sum: ['$myMeta.a', '$myMeta.b']}}}"),
                   fromjson("{$match: {myMeta: {$gte: 'abc'}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down both the project and internalize the remaining project, but we can't
    // push down the meta match due to the (now invalid) renaming.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {myMeta: {$sum: ['$meta.a', '$meta.b']}}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson(
            "{$_internalUnpackBucket: { include: ['_id', 'myMeta'], timeField: 'time', metaField: "
            "'myMeta', bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['myMeta']}}"),
        serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {myMeta: {$gte: 'abc'}}}"), serialized[2]);
}  // namespace

TEST_F(OptimizePipeline, ComputedProjectThenMatchNotPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {y: {$sum: ['$myMeta.a', '$myMeta.b']}}}"),
                   fromjson("{$match: {y: {$gt: 'abc'}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down the computed project but not the match, because it depends on the newly
    // computed values.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {y: {$sum: ['$meta.a', '$meta.b']}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'y'], timeField: 'time', metaField: "
                 "'myMeta', bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['y']}}"),
        serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {y: {$gt: 'abc'}}}"), serialized[2]);
}

TEST_F(OptimizePipeline, MetaSortThenProjectPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$sort: {myMeta: -1}}"),
                   fromjson("{$project: {myMeta: 1, x: 1}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down the $sort and internalize the $project.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {meta: -1}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'x', 'myMeta'], timeField: 'time', "
                 "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
        serialized[1]);
}

TEST_F(OptimizePipeline, ProjectThenMetaSortPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {myMeta: 1, x: 1}}"),
                   fromjson("{$sort: {myMeta: -1}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should internalize the $project and push down the $sort.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {meta: -1}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'x', 'myMeta'], timeField: 'time', "
                 "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
        serialized[1]);
}

TEST_F(OptimizePipeline, ComputedProjectThenSortPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {myMeta: '$myMeta.a'}}"),
                   fromjson("{$sort: {myMeta: 1}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down the $project and internalize the remaining project, but we can't do the
    // sort pushdown due to the renaming.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {myMeta: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson(
            "{$_internalUnpackBucket: { include: ['_id', 'myMeta'], timeField: 'time', metaField: "
            "'myMeta', bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['myMeta']}}"),
        serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {myMeta: 1}}"), serialized[2]);
}

TEST_F(OptimizePipeline, ExclusionProjectThenMatchPushDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time',"
                            "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {'myMeta.a': 0}}"),
                   fromjson("{$match: {'myMeta.b': {$lt: 10}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down the exclusion project on the metaField then push down the $match.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {'meta.b': {$lt: 10}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {meta: {a: false}, _id: true}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', "
                               "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                      serialized[2]);
}

TEST_F(OptimizePipeline, ExclusionProjectThenProjectPushDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time',"
                            "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {myMeta: 0}}"),
                   fromjson("{$project: {myMeta: 1, a: 1, _id: 0}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down the exclusion project on the metaField then internalize the remaining
    // project.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {meta: false, _id: true}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['a', 'myMeta'], timeField: 'time', "
                 "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
        serialized[1]);
}

TEST_F(OptimizePipeline, ProjectThenExclusionProjectPushDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time',"
                            "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {myMeta: 1, _id: 0}}"),
                   fromjson("{$project: {myMeta: 0}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should internalize one project then push down the remaining project on the metaField. Note
    // that we can push down an exclusion project on meta even after internalizing either kind of
    // project; swapping the order of simple projects does not affect the result.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {meta: false, _id: true}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['myMeta'], timeField: 'time', "
                               "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                      serialized[1]);
}

TEST_F(OptimizePipeline, ComputedProjectThenProjectPushDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {myMeta: '$myMeta.a'}}"),
                   fromjson("{$project: {myMeta: 0}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down the $project and internalize the remaining project. We should leave the
    // remaining project in the pipeline.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {myMeta: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson(
            "{$_internalUnpackBucket: { include: ['_id', 'myMeta'], timeField: 'time', metaField: "
            "'myMeta', bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['myMeta']}}"),
        serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {myMeta: false, _id: true}}"), serialized[2]);
}

TEST_F(OptimizePipeline, AddFieldsThenSortPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$addFields: {myMeta: '$myMeta.a'}}"),
                   fromjson("{$sort: {myMeta: 1}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->getSources().size());

    pipeline->optimizePipeline();

    // We should push down the $addFields, but we can't do the sort pushdown due to the renaming.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {myMeta: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['myMeta']}}"),
        serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {myMeta: 1}}"), serialized[2]);
}

TEST_F(OptimizePipeline, PushDownAddFieldsAndInternalizeProjection) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto addFieldsSpec = fromjson("{$addFields: {device: '$myMeta.a'}}");
    auto projectSpecObj = fromjson("{$project: {_id: true, x: true, device: true}}");

    auto pipeline =
        Pipeline::parse(makeVector(unpackSpecObj, addFieldsSpec, projectSpecObj), getExpCtx());

    pipeline->optimizePipeline();

    // We should push down the $addFields and internalize the $project.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {device: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['_id', 'device', 'x'], "
                               "timeField: 'time', metaField: 'myMeta', bucketMaxSpanSeconds: "
                               "3600, computedMetaProjFields: ['device']}}"),
                      serialized[1]);
}

TEST_F(OptimizePipeline, PushDownAddFieldsDoNotInternalizeProjection) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto addFieldsSpec = fromjson("{$addFields: {device: '$myMeta.a', z: {$add : ['$x', '$y']}}}");
    auto projectSpecObj = fromjson("{$project: {x: 1, device: 1, z : 1}}");

    auto pipeline =
        Pipeline::parse(makeVector(unpackSpecObj, addFieldsSpec, projectSpecObj), getExpCtx());

    pipeline->optimizePipeline();

    // We should split $addFields and push down the part depending on 'myMeta'. We cannot
    // internalize the $project.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(4u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {device: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['_id', 'device', 'x', 'y', "
                               "'z'], timeField: 'time', metaField: 'myMeta', "
                               "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['device']}}"),
                      serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {z: {$add : ['$x', '$y']}}}"), serialized[2]);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(
        kComparator.compare(fromjson("{$project: {_id: true, x: true, device: true, z: true}}"),
                            serialized[3]),
        0);
}

TEST_F(OptimizePipeline, InternalizeProjectAndPushdownAddFields) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj = fromjson("{$project: {x: true, y: true, myMeta: true}}");
    auto addFieldsSpec = fromjson("{$addFields: {newMeta: '$myMeta.a'}}");

    auto pipeline =
        Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj, addFieldsSpec), getExpCtx());

    pipeline->optimizePipeline();

    // We should internalize the $project and push down the $addFields.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {newMeta: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['_id', 'newMeta', 'x', 'y', "
                               "'myMeta'], timeField: 'time', metaField: 'myMeta', "
                               "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['newMeta']}}"),
                      serialized[1]);
}

TEST_F(OptimizePipeline, PushdownSortAndAddFields) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj,
                                               fromjson("{$addFields: {newMeta: '$myMeta.a'}}"),
                                               fromjson("{$sort: {myMeta: -1}}")),
                                    getExpCtx());

    pipeline->optimizePipeline();
    // We should push down both $sort and $addFields.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {newMeta: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {meta: -1}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['newMeta']}}"),
        serialized[2]);
}

TEST_F(OptimizePipeline, PushdownMatchAndAddFields) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(
        makeVector(unpackSpecObj,
                   fromjson("{$match: {'myMeta.a': {$eq: 'abc'}}}"),
                   fromjson("{$addFields: {newMeta: '$myMeta.b', z: {$add : ['$x', '$y']}}}")),
        getExpCtx());

    pipeline->optimizePipeline();

    // We should push down both the $match and the part of $addFields depending on 'myMeta'.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(4u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {'meta.a': {$eq: 'abc'}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {newMeta: '$meta.b'}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['newMeta']}}"),
        serialized[2]);
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {z: {$add : ['$x', '$y']}}}"), serialized[3]);
}

TEST_F(OptimizePipeline, MatchWithGeoWithinOnMeasurementsPushedDownUsingInternalBucketGeoWithin) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                            "'time', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {loc: {$geoWithin: {$geometry: {type: \"Polygon\", "
                            "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]}}}}}")),
        getExpCtx());

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(serialized.size(), 3U);

    // $match with $geoWithin on a non-metadata field is pushed down and $_internalBucketGeoWithin
    // is used.
    ASSERT_BSONOBJ_EQ(
        fromjson("{ $match: {$_internalBucketGeoWithin: { withinRegion: { $geometry: { type : "
                 "\"Polygon\" ,coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 "
                 "] ] ]}},field: \"loc\"}}}"),
        serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                               "'time', bucketMaxSpanSeconds: 3600}}"),
                      serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {loc: {$geoWithin: {$geometry: {type: \"Polygon\", "
                               "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]}}}}}"),
                      serialized[2]);
}

TEST_F(OptimizePipeline, MatchWithGeoWithinOnMetaFieldIsPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                            "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {'myMeta.loc': {$geoWithin: {$geometry: {type: \"Polygon\", "
                            "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]}}}}}")),
        getExpCtx());

    ASSERT_EQ(pipeline->getSources().size(), 2U);

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(serialized.size(), 2U);

    // $match with $geoWithin on the metadata field is pushed down without using
    // $_internalBucketGeoWithin.
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {'meta.loc': {$geoWithin: {$geometry: {type: \"Polygon\", "
                               "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]}}}}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                               "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                      serialized[1]);
}
}  // namespace
}  // namespace mongo
