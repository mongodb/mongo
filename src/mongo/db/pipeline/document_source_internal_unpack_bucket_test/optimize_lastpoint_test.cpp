// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using InternalUnpackBucketOptimizeLastpointTest = AggregationContextFixture;

void assertExpectedLastpointOpt(const boost::intrusive_ptr<ExpressionContext> expCtx,
                                const std::vector<std::string>& inputPipelineStrs,
                                const std::vector<std::string>& expectedPipelineStrs,
                                const bool expectedSuccess = true) {
    std::vector<BSONObj> inputPipelineBson;
    for (const auto& stageStr : inputPipelineStrs) {
        inputPipelineBson.emplace_back(fromjson(stageStr));
    }

    auto pipeline = pipeline_factory::makePipeline(
        inputPipelineBson, expCtx, pipeline_factory::kOptionsMinimal);
    auto& container = pipeline->getSources();
    ASSERT_EQ(container.size(), inputPipelineBson.size());

    // Assert that the lastpoint optimization succeeds/fails as expected.
    auto success = dynamic_cast<DocumentSourceInternalUnpackBucket*>(container.front().get())
                       ->optimizeLastpoint(container.begin(), &container);
    ASSERT_EQ(success, expectedSuccess);

    auto serialized = pipeline->serializeToBson();
    ASSERT_EQ(serialized.size(), expectedPipelineStrs.size());

    // Assert the pipeline is unchanged.
    auto serializedItr = serialized.begin();
    for (const auto& stageStr : expectedPipelineStrs) {
        auto expectedStageBson = fromjson(stageStr);
        ASSERT_BSONOBJ_EQ(*serializedItr, expectedStageBson);
        ++serializedItr;
    }
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest, NonLastpointDoesNotParticipateInOptimization) {
    auto assertPipelineUnoptimized = [&](const std::vector<std::string>& stageStrs) {
        assertExpectedLastpointOpt(getExpCtx(), stageStrs, stageStrs, /* expectedSuccess */ false);
    };

    // $sort must contain a time field.
    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$sort: {'m.a': 1}}",
         "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}, $willBeMerged: false}}"});

    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$top: {output: {b: '$b', c: '$c'}, sortBy: {'m.a': "
         "1}}}, $willBeMerged: false}}"});

    // $sort must have the time field as the last field in the sort key pattern.
    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$sort: {t: -1, 'm.a': 1}}",
         "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}, $willBeMerged: false}}"});

    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$top: {output: {b: '$b', c: '$c'}, sortBy: {t: -1, "
         "'m.a': 1}}}, $willBeMerged: false}}"});

    // $group's _id must be a meta field.
    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$sort: {'m.a': 1, t: -1}}",
         "{$group: {_id: '$nonMeta', b: {$first: '$b'}, c: {$first: '$c'}, $willBeMerged: "
         "false}}"});

    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$nonMeta', lastpoint: {$top: {output: {b: '$b', c: '$c'}, sortBy: "
         "{'m.a': 1, t: -1}}}, $willBeMerged: false}}"});

    // $group can only contain $first or $last accumulators or one $top/$bottom accumulator.
    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$sort: {'m.a': 1, t: -1}}",
         "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$last: '$c'}, $willBeMerged: false}}"});

    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$nonMeta', lastpoint1: {$top: {output: {b: '$b', c: '$c'}, sortBy: "
         "{'m.a': 1, t: -1}}}, lastpoint2: {$bottom: {output: {b: '$b', c: '$c'}, sortBy: "
         "{'m.a': 1, t: 1}}}, $willBeMerged: false}}"});

    // We disallow the rewrite for firstpoint queries due to rounding behaviour on control.min.time.
    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$sort: {'m.a': -1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}, $willBeMerged: false}}"});

    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$sort: {'m.a': -1, t: -1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}, $willBeMerged: false}}"});

    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$top: {output: {b: '$b', c: '$c'}, sortBy: {'m.a': 1, "
         "t: 1}}}, $willBeMerged: false}}"});

    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$bottom: {output: {b: '$b', c: '$c'}, sortBy: {'m.a': "
         "1, t: -1}}}, $willBeMerged: false}}"});

    // The _id field in $group's must match the meta field in $sort.
    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$sort: {'m.a': -1, t: -1}}",
         "{$group: {_id: '$m.z', b: {$first: '$b'}, c: {$first: '$c'}, $willBeMerged: false}}"});

    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.z', lastpoint: {$top: {output: {b: '$b', c: '$c'}, sortBy: {'m.a': 1, "
         "t: -1}}}, $willBeMerged: false}}"});

    // We cannot optimize for $topN or $bottomN with n != 1.
    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$topN: {n: {$const: 2}, output: {b: '$b', c: '$c'}, "
         "sortBy: {'m.a': 1, t: -1}}}, $willBeMerged: false}}"});

    assertPipelineUnoptimized(
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$bottomN: {n: {$const: 2}, output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': 1, t: 1}}}, $willBeMerged: false}}"});
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest,
       LastpointWithMetaSubfieldAscendingTimeDescending) {
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$sort: {'m.a': 1, t: -1}}",
         "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$sort: {'m.a': 1, t: -1}}",
         "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}, $willBeMerged: false}}"});
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$top: {output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': 1, t: -1}}}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$group: {_id: '$m.a', lastpoint: {$top: {output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': 1, t: -1}}}, $willBeMerged: false}}"});
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$topN: {n: {$const: 1}, output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': 1, t: -1}}}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$group: {_id: '$m.a', lastpoint: {$topN: {n: {$const: 1}, output: {b: '$b', c: '$c'}, "
         "sortBy: {'m.a': 1, t: -1}}}, $willBeMerged: false}}"});
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest,
       LastpointWithMetaSubfieldDescendingTimeDescending) {
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$sort: {'m.a': -1, t: -1}}",
         "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': -1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$sort: {'m.a': -1, t: -1}}",
         "{$group: {_id: '$m.a', b: {$first: '$b'}, c: {$first: '$c'}, $willBeMerged: false}}"});
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$top: {output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': -1, t: -1}}}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': -1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$group: {_id: '$m.a', lastpoint: {$top: {output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': -1, t: -1}}}, $willBeMerged: false}}"});
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$topN: {n: {$const: 1}, output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': -1, t: -1}}}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': -1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$group: {_id: '$m.a', lastpoint: {$topN: {n: {$const: 1}, output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': -1, t: -1}}}, $willBeMerged: false}}"});
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest, LastpointWithMetaSubfieldAscendingTimeAscending) {
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$sort: {'m.a': 1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': -1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$sort: {'m.a': 1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}, $willBeMerged: false}}"});
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$bottom: {output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': 1, t: 1}}}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': -1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$group: {_id: '$m.a', lastpoint: {$bottom: {output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': 1, t: 1}}}, $willBeMerged: false}}"});
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$bottomN: {n: {$const: 1}, output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': 1, t: 1}}}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': -1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$group: {_id: '$m.a', lastpoint: {$bottomN: {n: {$const: 1}, output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': 1, t: 1}}}, $willBeMerged: false}}"});
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest,
       LastpointWithMetaSubfieldDescendingTimeAscending) {
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$sort: {'m.a': -1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$sort: {'m.a': -1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}, $willBeMerged: false}}"});
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$bottom: {output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': -1, t: 1}}}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$group: {_id: '$m.a', lastpoint: {$bottom: {output: {b: '$b', c: "
         "'$c'}, sortBy: {'m.a': -1, t: 1}}}, $willBeMerged: false}}"});
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60}}",
         "{$group: {_id: '$m.a', lastpoint: {$bottomN: {n: {$const: 1}, "
         "output: {b: '$b', c: '$c'}, sortBy: {'m.a': -1, t: 1}}}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: "
         "{$first: '$control'}, data: {$first: '$data'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: "
         "'m', bucketMaxSpanSeconds: 60, sbeCompatible: false}}",
         "{$group: {_id: '$m.a', lastpoint: {$bottomN: {n: {$const: 1}, "
         "output: {b: '$b', c: '$c'}, sortBy: {'m.a': -1, t: 1}}}, $willBeMerged: false}}"});
}

TEST_F(InternalUnpackBucketOptimizeLastpointTest, LastpointWithComputedMetaProjectionFields) {
    // We might get such a case if $_internalUnpackBucket swaps with a $project. Verify that the
    // lastpoint optimization does not break in this scenario. Note that in the full pipeline we
    // would expect $_internalUnpackBucket to be preceded by a stage like $addFields. However,
    // optimizeLastpoint() only gets an interator to the $_internalUnpackBucket stage itself.
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60, computedMetaProjFields: ['abc', 'def']}}",
         "{$sort: {'m.a': -1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: {$first: '$control'}, data: "
         "{$first: '$data'}, abc: {$first: '$abc'}, def: {$first: '$def'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60, computedMetaProjFields: ['abc', 'def'], sbeCompatible: false}}",
         "{$sort: {'m.a': -1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}, $willBeMerged: false}}"});

    // Furthermore, validate that we can use the lastpoint optimization in the case where we have a
    // projection included in the final $group.
    assertExpectedLastpointOpt(
        getExpCtx(),
        /* inputPipelineStrs */
        {"{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60, computedMetaProjFields: ['abc', 'def']}}",
         "{$sort: {'m.a': -1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}, def: {$last: '$def'}}}"},
        /* expectedPipelineStrs */
        {"{$sort: {'meta.a': 1, 'control.max.t': -1, 'control.min.t': -1}}",
         "{$group: {_id: '$meta.a', meta: {$first: '$meta'}, control: {$first: '$control'}, data: "
         "{$first: '$data'}, abc: {$first: '$abc'}, def: {$first: '$def'}, $willBeMerged: false}}",
         "{$_internalUnpackBucket: {exclude: [], timeField: 't', metaField: 'm', "
         "bucketMaxSpanSeconds: 60, computedMetaProjFields: ['abc', 'def'], sbeCompatible: false}}",
         "{$sort: {'m.a': -1, t: 1}}",
         "{$group: {_id: '$m.a', b: {$last: '$b'}, c: {$last: '$c'}, def: {$last: '$def'}, "
         "$willBeMerged: false}}"});
}

}  // namespace
}  // namespace mongo
