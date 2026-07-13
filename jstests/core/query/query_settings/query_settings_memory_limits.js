/**
 * Tests that the accumulator memory limit knobs can be set via query settings: an override applies
 * to the matching query shape only, wins over the server parameter in both directions, appears in
 * explain with query settings attribution, and stops applying once the settings are removed.
 *
 * @tags: [
 *   # Runs setParameter on all non-config nodes.
 *   requires_non_retryable_commands,
 *   does_not_support_stepdowns,
 *   # Query settings commands can not be run on the shards directly.
 *   directly_against_shardsvrs_incompatible,
 *   # TODO(SERVER-113800): Enable setClusterParameters with replicaset started with --shardsvr
 *   transitioning_replicaset_incompatible,
 *   featureFlagPqsQueryKnobs,
 *   requires_fcv_90,
 *   # Query settings are matched by query shape; the implicit view redirect changes the shape so
 *   # the settings set on the collection query never apply.
 *   incompatible_with_views,
 *   # Several cases intentionally fail a command, which aborts the wrapping transaction in
 *   # multi-statement-transaction passthroughs and rolls back the test's data.
 *   does_not_support_transactions,
 *   assumes_balancer_off,
 * ]
 */
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getExplainCommand} from "jstests/libs/cmd_object_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const coll = db[jsTestName()];
const qsutils = new QuerySettingsUtils(db, coll.getName());

// The strings below are 22-25 bytes each. 'kLowLimitArray' is exceeded by accumulating all the
// documents' strings but not by the distinct subset; 'kLowLimitSet' is exceeded even by the
// distinct ones. Follows the calibration approach of
// jstests/noPassthrough/query/agg/agg_configurable_memory_limits.js, scaled up so that lowering
// the server parameter to these limits leaves headroom for background operations (e.g. the
// metadata consistency hook runs an internal $push aggregation of ~2KB).
const kStringSize = 22;
const kSetBaseline = 800;
const kLowLimitArray = (kSetBaseline * 5 * kStringSize) / 2;
const kLowLimitSet = ((kSetBaseline + 4) * kStringSize) / 2;
const kHighLimit = 100 * 1024 * 1024;

const testCases = [
    {
        wireName: "queryMaxPushBytes",
        serverParameter: "internalQueryMaxPushBytes",
        pipeline: [{$group: {_id: null, strings: {$push: "$y"}}}],
        lowLimit: kLowLimitArray,
    },
    {
        wireName: "queryMaxAddToSetBytes",
        serverParameter: "internalQueryMaxAddToSetBytes",
        pipeline: [{$group: {_id: null, strings: {$addToSet: "$y"}}}],
        lowLimit: kLowLimitSet,
    },
    {
        wireName: "queryMaxConcatArraysBytes",
        serverParameter: "internalQueryMaxConcatArraysBytes",
        pipeline: [{$group: {_id: null, strings: {$concatArrays: "$arr"}}}],
        lowLimit: kLowLimitArray,
    },
    {
        wireName: "queryMaxSetUnionBytes",
        serverParameter: "internalQueryMaxSetUnionBytes",
        pipeline: [{$group: {_id: null, strings: {$setUnion: "$arr"}}}],
        lowLimit: kLowLimitSet,
    },
    {
        wireName: "queryTopNAccumulatorBytes",
        serverParameter: "internalQueryTopNAccumulatorBytes",
        pipeline: [{$group: {_id: null, strings: {$firstN: {n: 2000, input: "$y"}}}}],
        lowLimit: kLowLimitSet,
    },
];

function runAggregation(pipeline) {
    return db.runCommand({aggregate: coll.getName(), pipeline, cursor: {}});
}

function getKnobsFromExplain(representativeQuery) {
    const explainCmd = getExplainCommand(qsutils.withoutDollarDB(representativeQuery));
    const explain = assert.commandWorked(db.runCommand(explainCmd));
    return {knobs: explain.queryKnobs ?? null, explain};
}

describe("query settings memory limit knobs", function () {
    before(function () {
        assertDropAndRecreateCollection(db, coll.getName());
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < kSetBaseline; i++) {
            for (let j = 0; j < 5; j++) {
                const y = j < 4 ? `string ${j}` : `string ${5 * i + 4}`;
                bulk.insert({_id: 5 * i + j, y: y, arr: [y]});
            }
        }
        assert.commandWorked(bulk.execute());
    });

    after(function () {
        assertDropCollection(db, coll.getName());
    });

    for (const c of testCases) {
        describe(c.wireName, function () {
            const representativeQuery = qsutils.makeAggregateQueryInstance({pipeline: c.pipeline});
            const lowSettings = {queryKnobs: {[c.wireName]: NumberLong(c.lowLimit)}};

            it("fails the matching query when the setting lowers the limit", function () {
                qsutils.withQuerySettings(representativeQuery, lowSettings, () => {
                    assert.commandFailedWithCode(
                        runAggregation(c.pipeline),
                        ErrorCodes.ExceededMemoryLimit,
                    );
                });
            });

            it("does not affect a query with a different shape", function () {
                qsutils.withQuerySettings(representativeQuery, lowSettings, () => {
                    assert.commandWorked(
                        runAggregation([{$match: {_id: {$gte: 0}}}, ...c.pipeline]),
                    );
                });
            });

            it("reports the overridden value in explain with settings attribution", function () {
                qsutils.withQuerySettings(representativeQuery, lowSettings, () => {
                    const {knobs, explain} = getKnobsFromExplain(representativeQuery);
                    const knob = knobs && knobs[c.wireName];
                    assert(knob, "expected knob in explain", {explain});
                    assert.eq(knob.source, "querySettings", "unexpected knob source", {explain});
                    assert.eq(Number(knob.value), c.lowLimit, "unexpected knob value", {explain});
                });
            });

            it("overrides a lowered server parameter upward", function () {
                runWithParamsAllNonConfigNodes(db, {[c.serverParameter]: c.lowLimit}, () => {
                    assert.commandFailedWithCode(
                        runAggregation(c.pipeline),
                        ErrorCodes.ExceededMemoryLimit,
                    );
                    qsutils.withQuerySettings(
                        representativeQuery,
                        {queryKnobs: {[c.wireName]: NumberLong(kHighLimit)}},
                        () => {
                            assert.commandWorked(runAggregation(c.pipeline));
                        },
                    );
                });
            });
        });
    }
});

// Constant folds that retain a footprint on the operation (parse-time optimize(), 'let' seeding)
// run before query settings are applied. Each query sets a per-operation limit override on its own
// shape, so the fold runs in a pending-settings window; the retained footprint stays charged and
// the first execution-time check must enforce the overridden limit against it. Enforcement needs
// the operation-parented trackers, so these cases only run with memory tracking enabled.
if (
    FeatureFlagUtil.isPresentAndEnabled(db, "QueryMemoryTracking") &&
    FeatureFlagUtil.isPresentAndEnabled(db, "ExpressionMemoryTracking")
) {
    describe("queryMaxMemoryUsageBytesPerOperation with constant folding", function () {
        // The fold is a large literal 'p' array (percentile values must lie in [0, 1]). $percentile
        // is a blocking stage that materializes the folded array and checks the operation limit on
        // the same node that folded it, so enforcement is deterministic across topologies. The
        // array must be large enough to clear the ~1MB CurOp chunking threshold (~3MB here).
        const kArraySize = 400 * 1000;
        const kPercentiles = Array.from({length: kArraySize}, (_, i) => (i + 1) / kArraySize);
        // The classic engine resolves the per-operation tracker; SBE pushdown does not.
        const makeSettings = (limit) => ({
            queryFramework: "classic",
            queryKnobs: {queryMaxMemoryUsageBytesPerOperation: NumberLong(limit)},
        });
        // A settings limit only affects the matching shape, so it can sit below the fold. The
        // restricted global also sits below it but well above any background operation's footprint.
        const kFoldLimit = 1 * 1024 * 1024;
        const kRestrictedLimit = 2 * 1024 * 1024;
        const kRelaxedLimit = 64 * 1024 * 1024;

        before(function () {
            assertDropAndRecreateCollection(db, coll.getName());
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, x: 1, y: "a", arr: ["a"]},
                    {_id: 1, x: 2, y: "b", arr: ["b"]},
                    {_id: 2, x: 3, y: "c", arr: ["c"]},
                ]),
            );
        });

        after(function () {
            assertDropCollection(db, coll.getName());
        });

        // Not every stage runs a limit check; this tracked evaluation guarantees each pipeline
        // gets one so the retained fold charge is enforced.
        const kTrackedCheckStage = {
            $addFields: {trackedCheck: {$concatArrays: [["tracked"], "$arr"]}},
        };

        // Which memory error surfaces depends on the stage that trips first: a non-spilling check
        // (the $addFields above) raises ExceededMemoryLimit, while a spill-capable $group on a
        // sharded collection raises QueryExceededMemoryLimitNoDiskUseAllowed. Both mean the
        // settings-lowered limit was enforced.
        const kMemoryLimitCodes = [
            ErrorCodes.ExceededMemoryLimit,
            ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed,
        ];

        const enforcedFoldCases = [
            {
                name: "a literal percentile 'p' array (ExpressionArray::optimize via parseP)",
                command: {
                    pipeline: [
                        {
                            $group: {
                                _id: null,
                                p: {
                                    $percentile: {
                                        p: kPercentiles,
                                        input: "$x",
                                        method: "approximate",
                                    },
                                },
                            },
                        },
                        kTrackedCheckStage,
                    ],
                },
            },
            {
                name: "a computed percentile 'p' array (ExpressionNary::optimize)",
                command: {
                    pipeline: [
                        {
                            $group: {
                                _id: null,
                                p: {
                                    $percentile: {
                                        p: {$concatArrays: [kPercentiles]},
                                        input: "$x",
                                        method: "approximate",
                                    },
                                },
                            },
                        },
                        kTrackedCheckStage,
                    ],
                },
            },
        ];

        function runCommand(command) {
            return db.runCommand({aggregate: coll.getName(), cursor: {}, ...command});
        }

        for (const c of enforcedFoldCases) {
            it(`fails ${c.name} when the setting lowers the operation limit`, function () {
                const representativeQuery = qsutils.makeAggregateQueryInstance(c.command);
                qsutils.withQuerySettings(representativeQuery, makeSettings(kFoldLimit), () => {
                    assert.commandFailedWithCode(runCommand(c.command), kMemoryLimitCodes);
                });
            });
        }

        it("passes the folded constant when the setting relaxes a lowered global", function () {
            const command = enforcedFoldCases[0].command;
            const representativeQuery = qsutils.makeAggregateQueryInstance(command);
            runWithParamsAllNonConfigNodes(
                db,
                {internalQueryMaxMemoryUsageBytesPerOperation: kRestrictedLimit},
                () => {
                    qsutils.withQuerySettings(
                        representativeQuery,
                        {queryFramework: "classic"},
                        () => {
                            assert.commandFailedWithCode(runCommand(command), kMemoryLimitCodes);
                        },
                    );
                    qsutils.withQuerySettings(
                        representativeQuery,
                        makeSettings(kRelaxedLimit),
                        () => {
                            assert.commandWorked(runCommand(command));
                        },
                    );
                },
            );
        });
    });
}
