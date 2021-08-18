/**
 * Test how $densify desugars.
 *
 * @tags: [
 *   # Needed as $densify is a 51 feature.
 *   requires_fcv_51,
 *   # We're testing the explain plan, not the query results, so the facet passthrough would fail.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */
(function() {
"use strict";

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagDensify: 1}))
        .featureFlagDensify.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the densify feature flag is disabled");
    return;
}

load("jstests/libs/fixture_helpers.js");

const coll = db[jsTestName()];
coll.insert({});

// Use .explain() to see what the stage desugars to.
// The result is formatted as explain-output, which differs from MQL syntax in some cases:
// for example {$sort: {a: 1}} explains as {$sort: {sortKey: {a: 1}}}.
function desugar(stage) {
    const result = coll.explain().aggregate([
        // prevent stages from being absorbed into the .find() layer
        {$_internalInhibitOptimization: {}},
        stage,
    ]);

    assert.commandWorked(result);
    // We proceed by cases based on topology.
    if (!FixtureHelpers.isMongos(db)) {
        assert(Array.isArray(result.stages), result);
        // The first two stages should be the .find() cursor and the inhibit-optimization stage;
        // the rest of the stages are what the user's 'stage' expanded to.
        assert(result.stages[0].$cursor, result);
        assert(result.stages[1].$_internalInhibitOptimization, result);
        return result.stages.slice(2);
    } else {
        if (result.splitPipeline) {
            assert(result.splitPipeline.shardsPart[0].$_internalInhibitOptimization, result);
            assert.eq(result.splitPipeline.shardsPart.length, 2);
            assert.eq(result.splitPipeline.mergerPart.length, 1);
            return [result.splitPipeline.shardsPart[1], result.splitPipeline.mergerPart[0]];
        } else if (result.stages) {
            // Required for aggregation_mongos_passthrough.
            assert(Array.isArray(result.stages), result);
            // The first two stages should be the .find() cursor and the inhibit-optimization stage;
            // the rest of the stages are what the user's 'stage' expanded to.
            assert(result.stages[0].$cursor, result);
            assert(result.stages[1].$_internalInhibitOptimization, result);
            return result.stages.slice(2);
        } else {
            // Required for aggregation_one_shard_sharded_collections.
            assert(Array.isArray(result.shards["shard-rs0"].stages), result);
            assert(result.shards["shard-rs0"].stages[0].$cursor, result);
            assert(result.shards["shard-rs0"].stages[1].$_internalInhibitOptimization, result);
            return result.shards["shard-rs0"].stages.slice(2);
        }
    }
}

// Implicit partition fields and sort are generated.
assert.eq(desugar({$densify: {field: "a", range: {step: 1.0, bounds: "full"}}}), [
    {$sort: {sortKey: {a: 1}}},
    {$_internalDensify: {field: "a", partitionByFields: [], range: {step: 1.0, bounds: "full"}}},
]);

// PartitionByFields are prepended to the sortKey if "partition" is specified.
assert.eq(
    desugar({
        $densify:
            {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: "partition"}}
    }),
    [
        {$sort: {sortKey: {b: 1, c: 1, a: 1}}},
        {
            $_internalDensify:
                {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: "partition"}}
        },
    ]);

// PartitionByFields are not prepended to the sortKey if "full" is specified.
assert.eq(
    desugar({
        $densify: {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: "full"}}
    }),
    [
        {$sort: {sortKey: {a: 1}}},
        {
            $_internalDensify:
                {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: "full"}}
        },
    ]);

// PartitionByFields are prepended to the sortKey if numeric bounds are specified.
assert.eq(
    desugar({
        $densify: {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: [-10, 0]}}
    }),
    [
        {$sort: {sortKey: {b: 1, c: 1, a: 1}}},
        {
            $_internalDensify:
                {field: "a", partitionByFields: ["b", "c"], range: {step: 1.0, bounds: [-10, 0]}}
        },
    ]);

// PartitionByFields are prepended to the sortKey if date bounds are specified.
assert.eq(
    desugar({
        $densify: {
            field: "a",
            partitionByFields: ["b", "c"],
            range:
                {step: 1.0, bounds: [new Date("2020-01-03"), new Date("2020-01-04")], unit: "day"}
        }
    }),
    [
        {$sort: {sortKey: {b: 1, c: 1, a: 1}}},
        {
            $_internalDensify: {
                field: "a",
                partitionByFields: ["b", "c"],
                range: {
                    step: 1.0,
                    bounds: [new Date("2020-01-03"), new Date("2020-01-04")],
                    unit: "day"
                }
            }
        },
    ]);
})();
