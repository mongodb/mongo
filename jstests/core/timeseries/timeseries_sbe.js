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
import {getAggPlanStages, getEngine} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeStatus, kFeatureFlagSbeFullEnabled, kSbeDisabled} from "jstests/libs/sbe_util.js";

// We pushdown unpack when checkSbeRestrictedOrFullyEnabled is true and when
// featureFlagTimeSeriesInSbe is set.

const sbeStatus = checkSbeStatus(db);
const sbeFullyEnabled = (sbeStatus == kFeatureFlagSbeFullEnabled);
const sbeUnpackPushdownEnabled =
    // SBE can't be disabled altogether.
    (sbeStatus != kSbeDisabled) &&
    (FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'TimeSeriesInSbe'));

// const sbeUnpackPushdownEnabled = checkSbeRestrictedOrFullyEnabled(db) &&
//     FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'TimeSeriesInSbe');

const coll = db.timeseries_sbe;
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
// The dataset doesn't matter, as we only care about the choice of the plan to execute the query.
assert.commandWorked(coll.insert({t: new Date(), m: 1, a: 42, b: 17}));

function runTest({pipeline, shouldUseSbe, aggStages}) {
    jsTestLog("Running pipeline " + tojson(pipeline));

    const explain = assert.commandWorked(coll.explain().aggregate(pipeline));
    const expectedEngine = shouldUseSbe ? "sbe" : "classic";
    assert.eq(expectedEngine,
              getEngine(explain),
              `SBE enabled. Should run ${tojson(pipeline)} in ${expectedEngine} but ran ${
                  tojson(explain)}`);

    if (aggStages) {
        for (let stage of aggStages) {
            let foundStages = getAggPlanStages(explain, stage);
            assert.neq(0,
                       foundStages.length,
                       () => "Expected to find " + stage + " in classic agg plan but ran " +
                           tojson(explain));
        }
    }
}

// 'kExclude' pipelines cannot be lowered to SBE no matter the flags.
runTest({
    pipeline: [{$match: {m: 17}}],
    shouldUseSbe: false,
});

// $project by itself is not lowered except in SBE full.
jsTestLog("ian: SBE full " + sbeFullyEnabled);
runTest({pipeline: [{$project: {a: 1, b: 1}}], shouldUseSbe: sbeFullyEnabled});

// $addFields, $project lowered only in SBE full.
runTest({
    pipeline:
        [{$addFields: {computedField: {$add: ["$a", 1]}}}, {$project: {computedField: 1, b: 1}}],
    shouldUseSbe: sbeFullyEnabled
});

// $match-inclusion $project is lowered when SBE is permitted.
runTest({
    pipeline: [{$match: {a: {$gt: 123}}}, {$project: {a: 1, b: 1}}],
    shouldUseSbe: sbeUnpackPushdownEnabled,
});

// $match-$addFields is never lowered.
runTest({
    pipeline: [{$match: {a: {$gt: 123}}}, {$addFields: {computedField: {$add: ["$a", 123]}}}],
    shouldUseSbe: false,
});

// $match-exclusion $project is never lowered.
runTest({
    pipeline: [{$match: {a: {$gt: 123}}}, {$project: {a: 0, b: 0}}],
    shouldUseSbe: false,
});

// Lone $group is lowered when SBE is permitted.
runTest({
    pipeline: [{$group: {_id: "$m", sum: {$sum: "$a"}}}],
    shouldUseSbe: sbeUnpackPushdownEnabled,
});

// $match-$group lowered when SBE is permitted.
runTest({
    pipeline: [{$match: {t: {$gt: new Date()}}}, {$group: {_id: "$m", sum: {$sum: "$a"}}}],
    shouldUseSbe: sbeUnpackPushdownEnabled,
});

// $match-$group, followed by a stage like $unwind. The $unwind will remain in classic.
runTest({
    pipeline: [
        {$match: {t: {$gt: new Date()}}},
        {$group: {_id: "$m", sum: {$sum: "$a"}}},
        {$unwind: "$x"}
    ],
    shouldUseSbe: sbeUnpackPushdownEnabled,
    aggStages: ["$unwind"]
});

// $match-$group, followed by a stage like $unwind. The $unwind will remain in classic.
runTest({
    pipeline: [
        {$match: {t: {$gt: new Date()}}},
        {$group: {_id: "$m", sum: {$sum: "$a"}}},
        {$group: {_id: "$sum"}}
    ],
    shouldUseSbe: sbeUnpackPushdownEnabled,
    aggStages: sbeFullyEnabled ? [] : ["$group"]
});

// $match-$group when $match contains filter on meta field.
runTest({
    pipeline: [{$match: {m: 123, t: {$gt: new Date()}}}, {$group: {_id: "$m", sum: {$sum: "$a"}}}],
    shouldUseSbe: sbeUnpackPushdownEnabled,
});

// $addFields-$match-$group permitted.
runTest({
    pipeline: [
        {$addFields: {computedField: {$add: ["$a", 1]}}},
        {$match: {computedField: 99.3}},
        {$group: {_id: null, s: {$sum: "$x"}}}
    ],
    shouldUseSbe: sbeUnpackPushdownEnabled,
});

// 'kInclude' pipelines cannot be lowered if there is a sort on the time field.
runTest({
    pipeline: [{$sort: {t: 1}}, {$project: {t: 1}}],
    shouldUseSbe: false,
});

// $match -> $addFields -> $group is permitted only in SBE full.
runTest({
    pipeline: [
        {$match: {a: {$gt: 1}}},
        {$addFields: {computedField: {$add: ["$a", 1]}}},
        {$group: {_id: null, s: {$sum: "$x"}}}
    ],
    shouldUseSbe: sbeFullyEnabled
});

// A stack of $project stages is permitted only in SBE full.
runTest({
    pipeline: [
        {"$project": {"_id": 0, "m": 1}},
        {"$project": {"_id": 0, "t": "$t"}},
        {"$project": {"_id": 1, "t": 1}}
    ],
    shouldUseSbe: sbeFullyEnabled,
});

// In most other cases the prefix of the pipeline, including bucket unpacking should be lowered to
// SBE. We'll sanity test a pipeline that should be lowered fully.
runTest({
    pipeline: [
        {$match: {t: {$lt: new Date()}}},
        {$group: {_id: "$a", n: {$sum: "$b"}}},
    ],
    shouldUseSbe: sbeUnpackPushdownEnabled,
});

// The full rewrite of a group might avoid unpacking. Let's check that these are fully lowered.
runTest({
    pipeline: [
        {$match: {m: {$in: [5, 15, 25]}}},
        {$group: {_id: "$m", min: {$min: "$a"}}},
    ],
    shouldUseSbe: sbeUnpackPushdownEnabled,
});

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
    shouldUseSbe: false
});

runTest({
    pipeline: [
        {$match: {time: {$gt: new Date()}}},
        {
            $group: {
                _id: {meta: "$m", a: "$a"},
                high: {$max: "$price"},
                low: {$min: "$price"},
            }
        },
        {
            $setWindowFields: {
                partitionBy: "$_id.meta",
                sortBy: {"_id.a": 1},
                output: {prev: {"$shift": {by: -1, output: "$low"}}}
            }
        }
    ],
    shouldUseSbe: sbeUnpackPushdownEnabled,

    // Everything should get pushed into SBE except setWindowFields.
    aggStages: sbeFullyEnabled ? [] : ["$_internalSetWindowFields"]
});
