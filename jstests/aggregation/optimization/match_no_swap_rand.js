/**
 * Test that $rand (and by extension, $sampleRate) doesn't get pushed down.
 * @tags: [
 *   # Tests the 'stages' field of the explain output which is hidden beneath each shard's name when
 *   # run against sharded collections.
 *   assumes_unsharded_collection,
 *   # Tests the explain output, so does not work when wrapped in a facet.
 *   do_not_wrap_aggregations_in_facets,
 *   # Explicitly testing optimization.
 *   requires_pipeline_optimization,
 *   # This test checks explain output exactly. In 7.2 an optimization was added to remove certain
 *   # imprecise predicates from the plan, so earlier versions witll have a slightly different
 *   # explain.
 *   requires_fcv_72
 * ]
 */
import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

function getWinningPlanForPipeline({coll, pipeline}) {
    const explain = assert.commandWorked(coll.explain().aggregate(pipeline));
    return getWinningPlanFromExplain(explain);
}

function assertScanFilterEq({coll, pipeline, filter}) {
    const winningPlan = getWinningPlanForPipeline({coll, pipeline});
    const collScan = getPlanStage(winningPlan, "COLLSCAN");
    assert(collScan);
    // Sometimes explain will have 'filter' set to an empty object, other times there will be no
    // 'filter'. If we are expecting there to be no filter on the COLLSCAN, either is acceptable.
    if (filter) {
        assert.docEq(filter, collScan.filter);
    } else {
        assert(!collScan.filter || Object.keys(collScan.filter).length == 0);
    }
}

// Test that a $match with a random expression should not be pushed past a $group.
{
    const coll = db[jsTestName()];
    coll.drop();

    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 100; i++) {
        bulk.insert({a: {b: i % 5, c: i}});
    }
    assert.commandWorked(bulk.execute());

    assertScanFilterEq({
        coll,
        pipeline: [{$group: {_id: "$a.b", first: {$first: "$$CURRENT"}}}, {$match: {$sampleRate: 0.25}}],
    });

    assertScanFilterEq({
        coll,
        pipeline: [
            {$group: {_id: "$a.b", first: {$first: "$$CURRENT"}}},
            {$replaceRoot: {newRoot: "$first"}},
            {$match: {$sampleRate: 0.25}},
        ],
    });

    assertScanFilterEq({
        coll,
        pipeline: [
            {$group: {_id: "$a.b", first: {$first: "$$CURRENT"}}},
            {$match: {$sampleRate: 0.25}},
            {$replaceRoot: {newRoot: "$first"}},
        ],
    });

    assertScanFilterEq({
        coll,
        pipeline: [
            {$match: {c: {$gt: 500}}},
            {$group: {_id: "$a.b", first: {$first: "$$CURRENT"}}},
            {$match: {$sampleRate: 0.25}},
        ],
        filter: {c: {$gt: 500}},
    });

    assertScanFilterEq({
        coll,
        pipeline: [
            {$match: {c: {$gt: 500}}},
            // A $lookup that split $match exprs can push down past.
            {
                $lookup: {
                    as: "joinedC",
                    from: coll.getName(),
                    localField: "c",
                    foreignField: "c",
                },
            },
            {
                $match: {
                    $and: [
                        {$expr: {$lt: ["$c", 800]}}, // Should split me out.
                        {$expr: {$lt: [{$rand: {}}, {$const: 0.25}]}}, // Can't split me out.
                    ],
                },
            },
        ],
        filter: {
            $and: [{c: {$gt: 500}}, {$expr: {$lt: ["$c", {$const: 800}]}}],
        },
    });
}

// Test that a $match with a random expression should not be pushed past $_internalUnpackBucket.
{
    const collName = jsTestName() + "_ts";
    db[collName].drop();
    assert.commandWorked(
        db.createCollection(collName, {
            timeseries: {
                timeField: "t",
                metaField: "m",
            },
        }),
    );
    const coll = db[collName];

    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 100; i++) {
        bulk.insert({t: new Date(), m: i});
    }
    assert.commandWorked(bulk.execute());

    assertScanFilterEq({
        coll,
        pipeline: [{$match: {$sampleRate: 0.25}}],
    });
}
