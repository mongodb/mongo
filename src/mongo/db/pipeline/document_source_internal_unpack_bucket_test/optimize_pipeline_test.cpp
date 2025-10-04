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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <vector>

namespace mongo {
namespace {

using OptimizePipeline = AggregationContextFixture;
const auto kExplain = SerializationOptions{
    .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};

TEST_F(OptimizePipeline, MixedMatchPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(
        makeVector(unpack, fromjson("{$match: {myMeta: {$gte: 0, $lte: 5}, a: {$lte: 4}}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->size());

    pipeline->optimizePipeline();

    // To get the optimized $match from the pipeline, we have to serialize with explain.
    auto stages = pipeline->writeExplainOps(kExplain);
    ASSERT_EQ(2u, stages.size());

    // We should push down the $match on the metaField and the predicates on the control field.
    // The created $match stages should be added before $_internalUnpackBucket and merged.
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{meta: {$gte: 0}}, {meta: {$lte: 5}},"
                               "{$or: [ {'control.min.a': {$_internalExprLte: 4}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ] } ] } } ] }]}}"),
                      stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{ $_internalUnpackBucket: { exclude: [], timeField: \"time\", "
                               "metaField: \"myMeta\", bucketMaxSpanSeconds: 3600, "
                               "eventFilter: { a: { $lte: 4 } } } }"),
                      stages[1].getDocument().toBson());

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), pipeline->serializeToBson());
}

TEST_F(OptimizePipeline, MetaMatchPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline =
        Pipeline::parse(makeVector(unpack, fromjson("{$match: {myMeta: {$gte: 0}}}")), getExpCtx());
    ASSERT_EQ(2u, pipeline->size());

    pipeline->optimizePipeline();

    // The $match on meta is moved before $_internalUnpackBucket and no other optimization is done.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$gte: 0}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(unpack, serialized[1]);

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(OptimizePipeline, MixedMatchOr) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto match = fromjson(
        "{$match: {$and: ["
        "  {x: {$lte: 1}},"
        "  {$or: ["
        // This $or is mixed: it contains both metadata and metric predicates.
        "    {'myMeta.a': {$gt: 1}},"
        "    {y: {$lt: 1}}"
        "  ]}"
        "]}}");
    auto pipeline = Pipeline::parse(makeVector(unpack, match), getExpCtx());
    ASSERT_EQ(2u, pipeline->size());

    pipeline->optimizePipeline();

    auto stages = pipeline->writeExplainOps(kExplain);
    ASSERT_EQ(2u, stages.size());
    auto expected = fromjson(
        "{$match: {$and: ["
        // Result of pushing down {x: {$lte: 1}}.
        "  {$or: ["
        "    {'control.min.x': {$_internalExprLte: 1}},"
        "    {$expr: {$ne: [ {$type: [ \"$control.min.x\" ]},"
        "                    {$type: [ \"$control.max.x\" ]}"
        "    ]}}"
        "  ]},"
        // Result of pushing down the $or predicate.
        "  {$or: ["
        "    {'meta.a': {$gt: 1}},"
        "    {'control.min.y': {$_internalExprLt: 1}},"
        "    {$expr: {$ne: [ {$type: [ \"$control.min.y\" ]},"
        "                    {$type: [ \"$control.max.y\" ]}"
        "    ]}}"
        "  ]}"
        "]}}");
    ASSERT_BSONOBJ_EQ(expected, stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{ $_internalUnpackBucket: { exclude: [], timeField: \"foo\", "
                               "metaField: \"myMeta\", bucketMaxSpanSeconds: 3600, "
                               "eventFilter: { $and: [ { x: { $lte: 1 } }, { $or: [ { "
                               "\"myMeta.a\": { $gt: 1 } }, { y: { $lt: 1 } } ] } ] } } }"),
                      stages[1].getDocument().toBson());

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), pipeline->serializeToBson());
}

TEST_F(OptimizePipeline, MixedMatchOnlyMetaMatchPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(
        makeVector(unpack,
                   fromjson("{$match: {myMeta: {$gte: 0, $lte: 5}, a: {$type: \"string\"}}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down the $match on the metaField but not the predicate on '$a', which is
    // ineligible because of the $type.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{meta: {$gte: 0}}, {meta: {$lte: 5}}]}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{ $_internalUnpackBucket: { exclude: [], timeField: \"time\", "
                               "metaField: \"myMeta\", bucketMaxSpanSeconds: 3600, "
                               "eventFilter: { a: { $type: [ 2 ] } } } }"),
                      serialized[1]);

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(OptimizePipeline, MultipleMatchesPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(makeVector(unpack,
                                               fromjson("{$match: {myMeta: {$gte: 0, $lte: 5}}}"),
                                               fromjson("{$match: {a: {$lte: 4}}}")),
                                    getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down both the $match on the metaField and the predicates on the control field.
    // The created $match stages should be added before $_internalUnpackBucket and merged.
    auto stages = pipeline->writeExplainOps(kExplain);
    ASSERT_EQ(2u, stages.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [ {meta: {$gte: 0}},"
                               "{meta: {$lte: 5}},"
                               "{$or: [ {'control.min.a': {$_internalExprLte: 4}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}}"),
                      stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{ $_internalUnpackBucket: { exclude: [], timeField: \"time\", "
                               "metaField: \"myMeta\", bucketMaxSpanSeconds: 3600, "
                               "eventFilter: { a: { $lte: 4 } } } }"),
                      stages[1].getDocument().toBson());

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), pipeline->serializeToBson());
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
    ASSERT_EQ(4u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down both the $match on the metaField and the predicates on the control field.
    // The created $match stages should be added before $_internalUnpackBucket and merged.
    auto stages = pipeline->writeExplainOps(kExplain);
    ASSERT_EQ(3u, stages.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [ { meta: { $gte: 0 } },"
                               "{meta: { $lte: 5 } },"
                               "{$or: [ { 'control.min.a': { $_internalExprLte: 4 } },"
                               "{$expr: { $ne: [ {$type: [ \"$control.min.a\" ] },"
                               "{$type: [ \"$control.max.a\" ] } ] } } ] }]}}"),
                      stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{ $_internalUnpackBucket: { exclude: [], timeField: \"time\", "
                               "metaField: \"myMeta\", bucketMaxSpanSeconds: 3600, "
                               "eventFilter: { a: { $lte: 4 } } } }"),
                      stages[1].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {sortKey: {a: 1}}}"), stages[2].getDocument().toBson());
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), pipeline->serializeToBson());
}

TEST_F(OptimizePipeline, MetaMatchThenCountPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {myMeta: {$eq: 'abc'}}}"),
                   fromjson("{$count: 'foo'}")),
        getExpCtx());
    ASSERT_EQ(4u, pipeline->size());  // $count is expanded into $group and $project.

    pipeline->optimizePipeline();

    // We should push down the $match and internalize the empty dependency set.
    auto serialized = pipeline->serializeToBson();
    // The $group is rewritten to make use of '$control.count' and the $unpack stage is removed.
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$eq: 'abc'}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$group: {_id: {$const: null}, foo: { $sum: { $cond: [ { $gte: [ "
                 "'$control.version', { $const: 2 } ] }, '$control.count', { $size: "
                 "[ { $objectToArray: ['$data.time']} ] } ] } }, $willBeMerged: false } }"),
        serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {foo: true, _id: false}}"), serialized[2]);
}

TEST_F(OptimizePipeline, SortThenMetaMatchPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(makeVector(unpack,
                                               fromjson("{$sort: {myMeta: -1}}"),
                                               fromjson("{$match: {myMeta: {$eq: 'abc'}}}")),
                                    getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

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
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down both the $sort and parts of the $match.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    auto expected = fromjson(
        "{$match: {$and: ["
        "  {meta: {$eq: 'abc'}},"
        "  {$or: ["
        "    {'control.max.a': {$_internalExprGte: 5}},"
        "    {$expr: {$ne: [ {$type: [ \"$control.min.a\" ]},"
        "                    {$type: [ \"$control.max.a\" ]}"
        "    ]}}"
        "  ]}"
        "]}}");
    ASSERT_BSONOBJ_EQ(expected, serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {meta: -1}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{ $_internalUnpackBucket: { exclude: [], timeField: \"time\", "
                               "metaField: \"myMeta\", bucketMaxSpanSeconds: 3600, "
                               "eventFilter: { a: { $gte: 5 } } } }"),
                      serialized[2]);
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(OptimizePipeline, MetaMatchThenSortPushedDown) {
    auto unpack = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(makeVector(unpack,
                                               fromjson("{$match: {myMeta: {$eq: 'abc'}}}"),
                                               fromjson("{$sort: {myMeta: -1}}")),
                                    getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

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
    ASSERT_EQ(3u, pipeline->size());

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
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    // We can push down part of the $match and use dependency analysis on the end of the pipeline.
    auto stages = pipeline->writeExplainOps(kExplain);
    ASSERT_EQ(3u, stages.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{meta: {$eq: 'abc'}},"
                               "{$or: [ {'control.min.a': { $_internalExprLte: 4 } },"
                               "{$expr: { $ne: [ {$type: [ \"$control.min.a\" ] },"
                               "{$type: [ \"$control.max.a\" ] } ] } } ] } ]}}"),
                      stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(
        fromjson("{ $_internalUnpackBucket: { include: [ \"_id\", \"a\", \"x\" ], timeField: "
                 "\"time\", metaField: \"myMeta\", bucketMaxSpanSeconds: 3600, "
                 "eventFilter: { a: { $lte: 4 } } } }"),
        stages[1].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {_id: true, x: true}}"),
                      stages[2].getDocument().toBson());

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), pipeline->serializeToBson());
}


TEST_F(OptimizePipeline, ProjectThenMetaMatchPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {x: 0}}"),
                   fromjson("{$match: {myMeta: {$eq: 'abc'}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

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
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down part of the $match and do dependency analysis on the rest.
    auto stages = pipeline->writeExplainOps(kExplain);
    ASSERT_EQ(3u, stages.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {$and: [{meta: {$eq: \"abc\"}},"
                               "{$or: [ {'control.min.a': {$_internalExprLte: 4}},"
                               "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ] },"
                               "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}}"),
                      stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(
        fromjson("{ $_internalUnpackBucket: { include: [ \"_id\", \"a\", \"x\", \"myMeta\" ], "
                 "timeField: \"time\", metaField: \"myMeta\", bucketMaxSpanSeconds: 3600, "
                 "eventFilter: { a: { $lte: 4 } } } }"),
        stages[1].getDocument().toBson());
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(
        kComparator.compare(fromjson("{$project: {_id: true, a: true, myMeta: true, x: true}}"),
                            stages[2].getDocument().toBson()),
        0);

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), pipeline->serializeToBson());
}

TEST_F(OptimizePipeline, ProjectWithRenameThenMixedMatchPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {myMeta: '$y', a: 1}}"),
                   fromjson("{$match: {myMeta: {$gte: 'abc'}, a: {$lte: 4}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down part of the $match and do dependency analysis on the end of the pipeline.
    auto stages = pipeline->writeExplainOps(kExplain);
    ASSERT_EQ(3u, stages.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$match: {$and: [{$or: [ {'control.max.y': {$_internalExprGte: \"abc\"}},"
                 "{$expr: {$ne: [ {$type: [ \"$control.min.y\" ]},"
                 "{$type: [ \"$control.max.y\" ]} ]}} ]},"
                 "{$or: [ {'control.min.a': {$_internalExprLte: 4}},"
                 "{$expr: {$ne: [ {$type: [ \"$control.min.a\" ] },"
                 "{$type: [ \"$control.max.a\" ]} ]}} ]} ]}}"),
        stages[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(
        fromjson("{ $_internalUnpackBucket: { include: [ \"_id\", \"a\", \"y\" ], timeField: "
                 "\"time\", metaField: \"myMeta\", bucketMaxSpanSeconds: 3600, "
                 "eventFilter: { $and: [ { y: { $gte: \"abc\" } }, { a: { $lte: 4 } } ] } } }"),
        stages[1].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {_id: true, a: true, myMeta: '$y'}}"),
                      stages[2].getDocument().toBson());
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), pipeline->serializeToBson());
}

TEST_F(OptimizePipeline, ComputedProjectThenMetaMatchPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {y: '$myMeta'}}"),
                   fromjson("{$match: {y: {$gte: 'abc'}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

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

TEST_F(OptimizePipeline, ComputedMetaProjectPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {a: {$sum: ['$myMeta.a', '$myMeta.b']}}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down the project.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {a: {$sum: ['$meta.a', '$meta.b']}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson(
            "{$_internalUnpackBucket: { include: [ '_id', 'a' ], timeField: 'time', "
            "metaField: 'myMeta', bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['a']}}"),
        serialized[1]);
}

TEST_F(OptimizePipeline, ComputedShadowingMetaProjectPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {myMeta: {$sum: ['$myMeta.a', '$myMeta.b']}}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down the project.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {meta: {$sum: ['$meta.a', '$meta.b']}}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: [ '_id', 'myMeta' ], timeField: 'time', "
                 "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
        serialized[1]);
}

TEST_F(OptimizePipeline, ComputedProjectThenMetaMatchPushedDownWithoutReorder) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {myMeta: {$sum: ['$myMeta.a', '$myMeta.b']}}}"),
                   fromjson("{$match: {myMeta: {$gte: 'abc'}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    // We should both push down the project and internalize the remaining project, but we can't
    // push down the meta match due to the (now invalid) renaming.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {meta: {$sum: ['$meta.a', '$meta.b']}}}"),
                      serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {meta: {$gte: 'abc'}}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: [ '_id', 'myMeta' ], timeField: 'time', "
                 "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
        serialized[2]);
}

TEST_F(OptimizePipeline, ComputedProjectThenMatchPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {y: {$sum: ['$myMeta.a', '$myMeta.b']}}}"),
                   fromjson("{$match: {y: {$gt: 'abc'}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down the computed project but not the match, because it depends on the newly
    // computed values.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {y: {$sum: ['$meta.a', '$meta.b']}}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: {include: ['_id', 'y'], timeField: "
                               "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600, "
                               "computedMetaProjFields: [ 'y' ]}}"),
                      serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {y: {$gt: 'abc'}}}"), serialized[2]);
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(OptimizePipeline, MetaSortThenProjectPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$sort: {myMeta: -1}}"),
                   fromjson("{$project: {myMeta: 1, x: 1}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

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
    ASSERT_EQ(3u, pipeline->size());

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
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down the $project and internalize the remaining project, but we can't do the
    // sort pushdown due to the renaming.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {meta: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {meta: 1}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(
        fromjson(
            "{$_internalUnpackBucket: { include: ['_id', 'myMeta'], timeField: 'time', metaField: "
            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
        serialized[2]);
    ;
}

TEST_F(OptimizePipeline, ExclusionProjectThenMatchPushDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time',"
                            "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {'myMeta.a': 0}}"),
                   fromjson("{$match: {'myMeta.b': {$lt: 10}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

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
    ASSERT_EQ(3u, pipeline->size());

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
    ASSERT_EQ(3u, pipeline->size());

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
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down the $project and internalize the remaining project. We should leave the
    // remaining project in the pipeline.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {meta: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {meta: false, _id: true}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(
        fromjson(
            "{$_internalUnpackBucket: { include: ['_id', 'myMeta'], timeField: 'time', metaField: "
            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
        serialized[2]);
}

TEST_F(OptimizePipeline, DoubleInclusionMetaProjectPushDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$project: {myMeta: 1, _id: 0}}"),
                   fromjson("{$project: {_id: 0, time: 1}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['myMeta'], timeField: 'time', metaField: "
                 "'myMeta', bucketMaxSpanSeconds: 3600}}"),
        serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {time: true, _id: false}}"), serialized[1]);

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(OptimizePipeline, ExcludeMetaProjectPushDown) {
    auto unpackSpec = fromjson(
        "{$_internalUnpackBucket: { exclude: ['myMeta'], timeField: 'time', metaField: "
        "'myMeta', bucketMaxSpanSeconds: 3600}}");
    auto pipeline = Pipeline::parse(
        makeVector(unpackSpec, fromjson("{$project: {_id: 0, time: 1}}")), getExpCtx());
    ASSERT_EQ(2u, pipeline->size());

    pipeline->optimizePipeline();
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpec, serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$project: {time: true, _id: false}}"), serialized[1]);

    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(OptimizePipeline, AddFieldsOfShadowingMetaPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$addFields: {myMeta: '$myMeta.a'}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down the $addFields and then the $sort.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {meta: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}"),
        serialized[1]);
}

TEST_F(OptimizePipeline, AddFieldsOfComputedMetaPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$addFields: {a: '$myMeta.a'}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down the $addFields and then the $sort.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {a: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['a']}}"),
        serialized[1]);
}

TEST_F(OptimizePipeline, AddFieldsThenSortPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$addFields: {myMeta: '$myMeta.a'}}"),
                   fromjson("{$sort: {myMeta: 1}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    // We should push down the $addFields and then the $sort.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(3u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {meta: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {meta: 1}}"), serialized[1]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
                 "bucketMaxSpanSeconds: 3600}}"),
        serialized[2]);
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
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['_id', 'device', 'x', 'y'"
                               "], timeField: 'time', metaField: 'myMeta', "
                               "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['device']}}"),
                      serialized[1]);
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {z: {$add : ['$x', '$y']}}}"), serialized[2]);
    const UnorderedFieldsBSONObjComparator kComparator;
    ASSERT_EQ(
        kComparator.compare(fromjson("{$project: {_id: true, x: true, device: true, z: true}}"),
                            serialized[3]),
        0);
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
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

TEST_F(OptimizePipeline, InternalizeProjectAndPushdownAddFieldsWithShadowingMeta) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600}}");
    auto projectSpecObj = fromjson("{$project: {x: true, y: true, myMeta: true}}");
    // The new 'myMeta' shadows the original 'myMeta'.
    auto addFieldsSpec = fromjson("{$addFields: {myMeta: '$myMeta.a'}}");

    auto pipeline =
        Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj, addFieldsSpec), getExpCtx());

    pipeline->optimizePipeline();

    // We should internalize the $project and push down the $addFields.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {meta: '$meta.a'}}"), serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'x', 'y', 'myMeta'], "
                 // The shadowing meta isn't considered as a computed meta and so no
                 // 'computedMetaProjFields'.
                 "timeField: 'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
        serialized[1]);
}

TEST_F(OptimizePipeline, DoNotSwapAddFieldsIfDependencyIsExcluded) {
    {
        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
            "bucketMaxSpanSeconds: 3600}}");
        auto projectSpecObj = fromjson("{$project: {x: true, _id: false}}");
        auto addFieldsSpec = fromjson("{$addFields: {newMeta: '$myMeta'}}");

        auto pipeline =
            Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj, addFieldsSpec), getExpCtx());

        pipeline->optimizePipeline();

        // We should internalize the $project but _not_ push down the $addFields because it's field
        // dependency has been excluded. Theoretically we could remove the $addFields for this
        // trivial except but not always.
        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(2u, serialized.size());
        ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['x'], timeField: 'time', "
                                   "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                          serialized[0]);
        ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {newMeta: '$myMeta'}}"), serialized[1]);
    }

    // Similar test except the dependency is on an excluded non-meta field.
    {
        auto unpackSpecObj = fromjson(
            "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: 'myMeta', "
            "bucketMaxSpanSeconds: 3600}}");
        auto projectSpecObj = fromjson("{$project: {x: true, _id: false}}");
        auto addFieldsSpec = fromjson("{$addFields: {newMeta: '$excluded'}}");

        auto pipeline =
            Pipeline::parse(makeVector(unpackSpecObj, projectSpecObj, addFieldsSpec), getExpCtx());

        pipeline->optimizePipeline();

        // We should internalize the $project but _not_ push down the $addFields because it's field
        // dependency has been excluded. Theoretically we could remove the $addFields for this
        // trivial except but not always.
        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(2u, serialized.size());
        ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: { include: ['x'], timeField: 'time', "
                                   "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                          serialized[0]);
        ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {newMeta: '$excluded'}}"), serialized[1]);
    }
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
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(OptimizePipeline, MatchWithGeoWithinOnMeasurementsPushedDownUsingInternalBucketGeoWithin) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                            "'time', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {loc: {$geoWithin: {$geometry: {type: \"Polygon\", "
                            "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]}}}}}")),
        getExpCtx());

    ASSERT_EQ(pipeline->size(), 2U);

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(serialized.size(), 2U);

    // $match with $geoWithin on a non-metadata field is pushed down and $_internalBucketGeoWithin
    // is used.
    ASSERT_BSONOBJ_EQ(
        fromjson("{ $match: {$_internalBucketGeoWithin: { withinRegion: { $geometry: { type : "
                 "\"Polygon\" ,coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 "
                 "] ] ]}},field: \"loc\"}}}"),
        serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{ $_internalUnpackBucket: { exclude: [], timeField: \"time\", "
                 "bucketMaxSpanSeconds: 3600, "
                 "eventFilter: { loc: { $geoWithin: { $geometry: { type: \"Polygon\", coordinates: "
                 "[ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ] } } } } } }"),
        serialized[1]);
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(OptimizePipeline, MatchWithGeoWithinOnMetaFieldIsPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                            "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {'myMeta.loc': {$geoWithin: {$geometry: {type: \"Polygon\", "
                            "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]}}}}}")),
        getExpCtx());

    ASSERT_EQ(pipeline->size(), 2U);

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

TEST_F(OptimizePipeline,
       MatchWithGeoIntersectsOnMeasurementsPushedDownUsingInternalBucketGeoWithin) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                            "'time', bucketMaxSpanSeconds: 3600}}"),
                   fromjson("{$match: {loc: {$geoIntersects: {$geometry: {type: \"Polygon\", "
                            "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]}}}}}")),
        getExpCtx());

    ASSERT_EQ(pipeline->size(), 2U);

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(serialized.size(), 2U);

    // $match with $geoIntersects on a non-metadata field is pushed down and
    // $_internalBucketGeoWithin is used.
    ASSERT_BSONOBJ_EQ(
        fromjson("{ $match: {$_internalBucketGeoWithin: { withinRegion: { $geometry: { type : "
                 "\"Polygon\" ,coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 "
                 "] ] ]}},field: \"loc\"}}}"),
        serialized[0]);
    ASSERT_BSONOBJ_EQ(
        fromjson("{ $_internalUnpackBucket: { exclude: [], timeField: \"time\", "
                 "bucketMaxSpanSeconds: 3600, "
                 "eventFilter: { loc: { $geoIntersects: { $geometry: { type: \"Polygon\", "
                 "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ] } } } } } }"),
        serialized[1]);
}

TEST_F(OptimizePipeline, MatchWithGeoIntersectsOnMetaFieldIsPushedDown) {
    auto pipeline = Pipeline::parse(
        makeVector(
            fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                     "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
            fromjson("{$match: {'myMeta.loc': {$geoIntersects: {$geometry: {type: \"Polygon\", "
                     "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]}}}}}")),
        getExpCtx());

    ASSERT_EQ(pipeline->size(), 2U);

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(serialized.size(), 2U);

    // $match with $geoIntersects on the metadata field is pushed down without using
    // $_internalBucketGeoWithin.
    ASSERT_BSONOBJ_EQ(
        fromjson("{$match: {'meta.loc': {$geoIntersects: {$geometry: {type: \"Polygon\", "
                 "coordinates: [ [ [ 0, 0 ], [ 3, 6 ], [ 6, 1 ], [ 0, 0 ] ] ]}}}}}"),
        serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$_internalUnpackBucket: {exclude: [], timeField: "
                               "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}"),
                      serialized[1]);
}

TEST_F(OptimizePipeline, StreamingGroupIsEnabledWhenPossible) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: {exclude: [], timeField: "
        "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}");
    auto groupSpecObj = fromjson(
        "{$group: {_id: {hour: {$dateTrunc: {date: '$time', unit: 'hour'}}, symbol: "
        "'$myMeta.symbol'}"
        ", 'sum': {$sum: '$tradeAmount'}}}");
    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj,
                                               fromjson("{$sort: {time: 1}}"),
                                               fromjson("{$match: {'tradePrice': 100}}"),
                                               groupSpecObj),
                                    getExpCtx());

    ASSERT_EQ(pipeline->size(), 4U);

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(serialized.size(), 4U);

    auto streamingGroupSpecObj = fromjson(
        "{$_internalStreamingGroup: {_id: {hour: {$dateTrunc: {date: '$time', unit: {$const: "
        "'hour'}}}, symbol: '$myMeta.symbol'}, 'sum': {$sum: '$tradeAmount'}, '$willBeMerged': "
        "false, '$monotonicIdFields': ['hour']}}");
    ASSERT_BSONOBJ_EQ(streamingGroupSpecObj, serialized.back());
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(OptimizePipeline, StreamingGroupIsNotEnabledWhenTimeFieldIsModified) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: {exclude: [], timeField: "
        "'time', metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}");
    auto groupSpecObj = fromjson(
        "{$group: {_id: {hour: '$time', symbol: '$myMeta.symbol'}, 'sum': {$sum: "
        "'$tradeAmount'}, $willBeMerged: false}}");
    auto pipeline = Pipeline::parse(
        makeVector(unpackSpecObj,
                   fromjson("{$addFields: {'time': {$dateTrunc: {date: '$time', unit: 'hour'}}}}"),
                   fromjson("{$sort: {time: 1}}"),
                   groupSpecObj),
        getExpCtx());

    ASSERT_EQ(pipeline->size(), 4U);

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(serialized.size(), 4U);
    ASSERT_BSONOBJ_EQ(groupSpecObj, serialized.back());
}

TEST_F(OptimizePipeline, ComputedMetaProjFieldsAreNotInInclusionProjection) {
    auto pipeline = Pipeline::parse(
        makeVector(
            fromjson(
                "{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                "'myMeta', bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['time', 'y']}}"),
            fromjson("{$project: {time: 1, x: 1}}")),
        getExpCtx());
    ASSERT_EQ(2u, pipeline->size());

    pipeline->optimizePipeline();

    // The fields in 'computedMetaProjFields' that are not in the project should be removed.
    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(
        fromjson("{$_internalUnpackBucket: { include: ['_id', 'time', 'x'], timeField: 'time', "
                 "metaField: "
                 "'myMeta', bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['time']}}"),
        serialized[0]);
    makePipelineOptimizeAssertNoRewrites(getExpCtx(), serialized);
}

TEST_F(OptimizePipeline, ComputedMetaProjectFieldsAfterInclusionGetsAddedToIncludes) {

    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600, computedMetaProjFields: []}}"),
                   fromjson("{$project: {myMeta: 1}}"),
                   fromjson("{$addFields: {newMeta: {$toUpper : '$myMeta'}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());

    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {newMeta: {$toUpper: ['$meta']}}}"), serialized[0]);

    // 'newMeta' field gets added to 'computedMetaProjFields' and to 'include'.
    auto expectedSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['_id','newMeta', 'myMeta'], timeField: 'time', "
        "metaField: 'myMeta', "
        "bucketMaxSpanSeconds: 3600, computedMetaProjFields: ['newMeta']}}");
    ASSERT_BSONOBJ_EQ(expectedSpecObj, serialized[1]);
}

TEST_F(OptimizePipeline, ShadowingMetaProjectFieldsAfterInclusionGetsAddedToIncludes) {
    auto pipeline = Pipeline::parse(
        makeVector(fromjson("{$_internalUnpackBucket: { exclude: [], timeField: 'time', metaField: "
                            "'myMeta', bucketMaxSpanSeconds: 3600, computedMetaProjFields: []}}"),
                   fromjson("{$project: {myMeta: 1}}"),
                   // The new 'myMeta' shadows the original 'myMeta'.
                   fromjson("{$addFields: {myMeta: {$toUpper : '$myMeta'}}}")),
        getExpCtx());
    ASSERT_EQ(3u, pipeline->size());

    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(2u, serialized.size());

    ASSERT_BSONOBJ_EQ(fromjson("{$addFields: {meta: {$toUpper: ['$meta']}}}"), serialized[0]);

    // 'newMeta' field gets added to 'computedMetaProjFields' and to 'include'.
    auto expectedSpecObj = fromjson(
        "{$_internalUnpackBucket: { include: ['_id', 'myMeta'], timeField: 'time', "
        // The shadowing meta isn't considered as a computed meta and so no
        // 'computedMetaProjFields'.
        "metaField: 'myMeta', bucketMaxSpanSeconds: 3600}}");
    ASSERT_BSONOBJ_EQ(expectedSpecObj, serialized[1]);
}
}  // namespace
}  // namespace mongo
