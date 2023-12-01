/**
 * Tests that we do lower queries over time-series to SBE when the corresponding flags are enabled.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # During fcv upgrade/downgrade the engine might not be what we expect.
 *   cannot_run_during_upgrade_downgrade,
 *   # "Explain of a resolved view must be executed by mongos"
 *   directly_against_shardsvrs_incompatible,
 *   # Some suites use mixed-binary cluster setup where some nodes might have the flag enabled while
 *   # others -- not. For this test we need control over whether the flag is set on the node that
 *   # ends up executing the query.
 *   assumes_standalone_mongod
 * ]
 */
import {getAggPlanStage, getEngine} from "jstests/libs/analyze_plan.js";
import {checkSBEEnabled} from "jstests/libs/sbe_util.js";

const sbeEnabled = checkSBEEnabled(db, ["featureFlagTimeSeriesInSbe"]);

const coll = db.timeseries_sbe;
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
// The dataset doesn't matter, as we only care about the choice of the plan to execute the query.
assert.commandWorked(coll.insert({t: new Date(), m: 1, a: 42, b: 17}));

function runTest({pipeline, expectedEngine}) {
    const explain = assert.commandWorked(coll.explain().aggregate(pipeline));

    if (sbeEnabled) {
        assert.eq(expectedEngine,
                  getEngine(explain),
                  `SBE enabled. Should run ${tojson(pipeline)} in ${expectedEngine} but ran ${
                      tojson(explain)}`);

        // The tests in this file are such that the whole pipeline is expected to be lowered. If
        // adding hybrid pipelines, extend this check.
        if (expectedEngine === "sbe") {
            assert.eq(
                null,
                getAggPlanStage(explain, "$cursor"),
                `SBE enabled. Should run ${
                    tojson(pipeline)} fully in the query planner layer but ran ${tojson(explain)}`);
        }
    } else {
        // It has to be classic.
        assert.eq("classic",
                  getEngine(explain),
                  `SBE not enabled. Should run ${tojson(pipeline)} in classic but ran ${
                      tojson(explain)}`);
    }
}

// 'kExclude' pipelines cannot be lowered to SBE no matter the flags.
runTest({
    pipeline: [{$match: {m: 17}}],
    expectedEngine: "classic",
})

// 'kInclude' pipelines cannot be lowered if there is a sort on the time field.
runTest({
    pipeline: [{$sort: {t: 1}}, {$project: {t: 1}}],
    expectedEngine: "classic",
})

// In most other cases the prefix of the pipeline, including bucket unpacking should be lowered to
// SBE. We'll sanity test a pipeline that should be lowered fully.
runTest({
    pipeline: [
        {$project: {_id: 0}},
        {$sort: {m: 1}},
        {$match: {t: {$lt: new Date()}}},
        {$group: {_id: "$a", n: {$sum: "$b"}}},
    ],
    expectedEngine: "sbe",
})

// The full rewrite of a group might avoid unpacking. Let's check that these are fully lowered.
runTest({
    pipeline: [
        {$match: {m: {$in: [5, 15, 25]}}},
        {$group: {_id: "$m", min: {$min: "$a"}}},
    ],
    expectedEngine: "sbe",
})

// Bucket unpacking should not be lowered when there is an eventFilter with a full match
// expression that is not supported in SBE. This entire pipeline should run in classic.
runTest({
    pipeline: [
        {
            $match: {
                a: {
                    // $geoWithin is not supported in SBE.
                    $geoWithin: {
                        $geometry:
                            {type: "Polygon", coordinates: [[[0, 0], [3, 6], [6, 1], [0, 0]]]}
                    }
                }
            }
        },
        {$project: {t: 1}}
    ],
    expectedEngine: "classic",
});
