/**
 * Tests the V3 explain against a sharded cluster, covering the following scenarios:
 *
 *  1. An unsharded collection, where the whole operation runs on the primary shard.
 *  2. A sharded collection with a shard-key predicate, so exactly one shard is targeted.
 *  3. A sharded collection with a scatter-gather query, including both an unsorted and a sorted
 *     merge (SHARD_MERGE vs. SHARD_MERGE_SORT on the find path), and for aggregation both a merge
 *     running on mongos and the merge forced to run on a shard.
 *  4. A query with only one viable candidate plan (no multi-planning), to exercise the
 *     "singlePlan" ranker choice through mongos.
 *
 * @tags: [
 *  multiversion_incompatible,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    getAllNodeExplains,
    getQueryPlanner,
    assertChosenRanker,
} from "jstests/libs/query/analyze_plan.js";
import {
    getPlanRankerConfig,
    setPlanRankerConfigOnAllNonConfigNodes,
} from "jstests/libs/query/cbr_utils.js";
import {
    assertV3QueryPlanner,
    hasCostBasedGroup,
    kPlannerStatsVerbosities,
    kPlanPhaseGroups,
} from "jstests/libs/query/explain_v3_helpers.js";
import {
    checkSbeCompletelyDisabled,
    checkSbeFullyEnabled,
    isDeferredGetExecutorEnabled,
} from "jstests/libs/query/sbe_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kV3Verbosities = ["plannerChoice", "plannerStats", "execStats"];

// The only V3 verbosity that executes, and so the only one reporting execution statistics.
const kExecutingVerbosity = "execStats";

const dbName = jsTestName();
const kUnshardedCollName = "unsharded";
const kShardedCollName = "sharded";

let st;
let db;

const kRankerModes = [
    {
        name: "default configuration",
        forcedConfig: null,
        chosenRanker: "multiPlanning",
        // All test cases in this file early exit but in general this is not always true.
        reason: "mpEarlyExit",
    },
    {
        name: "the cost-based ranker forced",
        forcedConfig: {internalQueryPlanRanker: "costBased"},
        chosenRanker: "costBased",
        reason: "queryPlanRankerKnob",
        // TODO SERVER-130973: Remove this once CBR supports SBE.
        skipOnFullSbe: true,
    },
    {
        name: "multi-planning forced",
        forcedConfig: {internalQueryPlanRanker: "multiPlanning"},
        chosenRanker: "multiPlanning",
        reason: "queryPlanRankerKnob",
    },
];

/**
 * Returns true if the given shard agg explain contains execution statistics.
 */
function aggHasExecutionStats(shardSection) {
    const owner = shardSection.hasOwnProperty("queryPlanner")
        ? shardSection
        : shardSection.stages[0].$cursor;
    return owner.hasOwnProperty("executionStats");
}

/**
 * Returns the nReturned from the given shard agg explain's $cursor statistics.
 */
function aggExecutionStatsNReturned(shardSection) {
    const owner = shardSection.hasOwnProperty("queryPlanner")
        ? shardSection
        : shardSection.stages[0].$cursor;
    return owner.executionStats.nReturned;
}

/**
 * Runs 'testCallback(mode, skipIfCbrUnsupported, skipIfForcedClassicEngine)' once per entry of
 * 'kRankerModes', each in its own nested describe that forces (and restores) that mode's ranker
 * configuration on every shard around it. 'testCallback' should call it() to register its test
 * cases against the active 'mode'.
 */
function describeUnderEachRankerMode(testCallback) {
    for (const configuredMode of kRankerModes) {
        const mode = {...configuredMode};
        describe(mode.name, function () {
            let savedRankerConfig;
            let cbrUnsupported = false;
            let classicEngineForced = false;

            before(function () {
                classicEngineForced = checkSbeCompletelyDisabled(st.shard0.getDB(dbName));
                // TODO SERVER-130179: The SBE explainer does not record the ranker choice reason
                // when deferred engine choice is off, so only the chosen ranker can be checked.
                const shardDB = st.shard0.getDB(dbName);
                if (checkSbeFullyEnabled(shardDB) && !isDeferredGetExecutorEnabled(shardDB)) {
                    mode.reason = undefined;
                }
                if (mode.skipOnFullSbe && checkSbeFullyEnabled(st.shard0.getDB(dbName))) {
                    cbrUnsupported = true;
                    return;
                }
                assert.commandWorked(db.runCommand({planCacheClear: kUnshardedCollName}));
                assert.commandWorked(db.runCommand({planCacheClear: kShardedCollName}));
                if (mode.forcedConfig) {
                    // Every shard in this fixture starts with the same ranker configuration
                    // (they're all given the same startup options), so it's enough to capture it
                    // from one.
                    savedRankerConfig = getPlanRankerConfig(st.rs0.getPrimary().getDB("admin"));
                    setPlanRankerConfigOnAllNonConfigNodes(st.s, mode.forcedConfig);
                }
            });

            after(function () {
                // Restore each shard's original configuration rather than
                // setPlanRankerConfig()'s hard-coded defaults, since a variant may have started
                // this suite with different ranker parameters.
                if (savedRankerConfig) {
                    setPlanRankerConfigOnAllNonConfigNodes(st.s, savedRankerConfig);
                }
            });

            const skipIfCbrUnsupported = () => {
                if (cbrUnsupported) {
                    jsTest.log.info("Skipping test: CBR does not support SBE");
                }
                return cbrUnsupported;
            };

            const skipIfForcedClassicEngine = () => {
                if (classicEngineForced) {
                    jsTest.log.info(
                        "Skipping test: classic engine is forced, so a shard's agg pipeline " +
                            "may not collapse into the find layer, changing what nReturned means",
                    );
                }
                return classicEngineForced;
            };

            testCallback(mode, skipIfCbrUnsupported, skipIfForcedClassicEngine);
        });
    }
}

describe("V3 explain in a sharded cluster", function () {
    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 1},
        });
        db = st.s.getDB(dbName);

        // Ensure shard0 is primary.
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
        );

        const unshardedColl = db[kUnshardedCollName];
        assert.commandWorked(unshardedColl.createIndex({a: 1}));
        assert.commandWorked(unshardedColl.createIndex({b: 1}));

        const shardedColl = db[kShardedCollName];
        assert.commandWorked(shardedColl.createIndex({sk: 1}));
        assert.commandWorked(shardedColl.createIndex({a: 1}));
        assert.commandWorked(shardedColl.createIndex({b: 1}));

        st.shardColl(shardedColl, {sk: 1}, {sk: 10}, {sk: 15});

        const docs = [];
        for (let i = 0; i < 20; i++) {
            docs.push({sk: i, a: i % 4, b: i % 5});
        }
        assert.commandWorked(unshardedColl.insert(docs));
        assert.commandWorked(shardedColl.insert(docs));
    });

    after(function () {
        st.stop();
    });

    // A predicate on two indexed fields, so the shards multi-plan between the {a: 1} and {b: 1}
    // index scans.
    const multiPlanFilter = {a: {$gte: 0}, b: {$gte: 0}};

    describe("on an unsharded collection in a sharded cluster", function () {
        function runUnshardedTests(mode, skipIfCbrUnsupported, skipIfForcedClassicEngine) {
            for (const verbosity of kV3Verbosities) {
                it(`single shard's find explain at ${verbosity}`, function () {
                    if (skipIfCbrUnsupported()) {
                        return;
                    }
                    const explain = assert.commandWorked(
                        db.runCommand({
                            explain: {find: kUnshardedCollName, filter: multiPlanFilter},
                            verbosity,
                        }),
                    );

                    assert.eq(
                        explain.queryPlanner.winningPlan.stage,
                        "SINGLE_SHARD",
                        "an unsharded collection should not merge from shards",
                        {explain},
                    );

                    const shardSections = getAllNodeExplains(explain);
                    assert.eq(shardSections.length, 1, "expected exactly one shard", {explain});
                    assert.eq(shardSections[0].shardName, st.shard0.shardName, "wrong shard", {
                        explain,
                    });
                    assert.eq(shardSections[0].explainVersion, "3", "expected explainVersion '3'", {
                        explain,
                    });
                    assertV3QueryPlanner(shardSections[0], verbosity, {explain});
                    assertChosenRanker(explain, mode.chosenRanker, mode.reason);

                    // The find path aggregates the shards' execution statistics into a single
                    // router-level section.
                    assert.eq(
                        explain.hasOwnProperty("executionStats"),
                        verbosity === kExecutingVerbosity,
                        "unexpected executionStats presence",
                        {explain},
                    );
                    if (verbosity === kExecutingVerbosity) {
                        assert.eq(
                            explain.executionStats.nReturned,
                            20,
                            "expected every document in the unsharded collection to match",
                            {explain},
                        );
                    }
                });

                it(`single shard's agg explain at ${verbosity}`, function () {
                    if (skipIfCbrUnsupported() || skipIfForcedClassicEngine()) {
                        return;
                    }
                    const explain = assert.commandWorked(
                        db.runCommand({
                            explain: {
                                aggregate: kUnshardedCollName,
                                pipeline: [
                                    {$match: multiPlanFilter},
                                    {$group: {_id: "$a", c: {$sum: 1}}},
                                ],
                                cursor: {},
                            },
                            verbosity,
                        }),
                    );

                    assert(!explain.splitPipeline, "unexpected split pipeline", {explain});

                    const shardSections = getAllNodeExplains(explain);
                    assert.eq(shardSections.length, 1, "expected exactly one shard", {explain});
                    assert.eq(shardSections[0].host, st.rs0.getPrimary().host, "wrong shard", {
                        explain,
                    });
                    assert.eq(shardSections[0].explainVersion, "3", "expected explainVersion '3'", {
                        explain,
                    });
                    assertV3QueryPlanner(getQueryPlanner(shardSections[0]), verbosity, {explain});
                    assertChosenRanker(explain, mode.chosenRanker, mode.reason);
                    assert.eq(
                        aggHasExecutionStats(shardSections[0]),
                        verbosity === kExecutingVerbosity,
                        "unexpected executionStats presence",
                        {explain},
                    );
                    if (verbosity === kExecutingVerbosity) {
                        // The whole pipeline runs on this one shard, so nReturned here is the
                        // $group's own output cardinality: one row per distinct 'a' value (0-3).
                        assert.eq(
                            aggExecutionStatsNReturned(shardSections[0]),
                            4,
                            "expected one group per distinct 'a' value",
                            {explain},
                        );
                    }
                });
            }
        }

        describeUnderEachRankerMode(runUnshardedTests);
    });

    describe("on a sharded collection with a shard-targeted query", function () {
        // An equality on the shard key, in the range owned by shard0.
        const targetedFilter = Object.assign({sk: 3}, multiPlanFilter);

        function runShardTargetedTests(mode, skipIfCbrUnsupported, skipIfForcedClassicEngine) {
            for (const verbosity of kV3Verbosities) {
                it(`targets one shard and keeps the V3 find shape at ${verbosity}`, function () {
                    if (skipIfCbrUnsupported()) {
                        return;
                    }
                    const explain = assert.commandWorked(
                        db.runCommand({
                            explain: {find: kShardedCollName, filter: targetedFilter},
                            verbosity,
                        }),
                    );

                    assert.eq(
                        explain.queryPlanner.winningPlan.stage,
                        "SINGLE_SHARD",
                        "a shard-key equality should target a single shard",
                        {explain},
                    );

                    const shardSections = getAllNodeExplains(explain);
                    assert.eq(shardSections.length, 1, "expected exactly one targeted shard", {
                        explain,
                    });
                    assert.eq(
                        shardSections[0].shardName,
                        st.shard0.shardName,
                        "wrong shard targeted",
                        {explain},
                    );
                    assert.eq(shardSections[0].explainVersion, "3", "expected explainVersion '3'", {
                        explain,
                    });
                    assertV3QueryPlanner(shardSections[0], verbosity, {explain});
                    assertChosenRanker(explain, mode.chosenRanker, mode.reason);
                    assert.eq(
                        explain.hasOwnProperty("executionStats"),
                        verbosity === kExecutingVerbosity,
                        "unexpected executionStats presence",
                        {explain},
                    );
                    if (verbosity === kExecutingVerbosity) {
                        assert.eq(
                            explain.executionStats.nReturned,
                            1,
                            "expected only the shard-key-matching document",
                            {explain},
                        );
                    }
                });

                it(`targets one shard and keeps the V3 agg shape at ${verbosity}`, function () {
                    if (skipIfCbrUnsupported() || skipIfForcedClassicEngine()) {
                        return;
                    }
                    const explain = assert.commandWorked(
                        db.runCommand({
                            explain: {
                                aggregate: kShardedCollName,
                                pipeline: [
                                    {$match: targetedFilter},
                                    {$group: {_id: "$a", c: {$sum: 1}}},
                                ],
                                cursor: {},
                            },
                            verbosity,
                        }),
                    );

                    // Targeting a single shard means there is nothing to merge, so the pipeline is
                    // not split.
                    assert(!explain.splitPipeline, "unexpected split pipeline", {explain});

                    const shardSections = getAllNodeExplains(explain);
                    assert.eq(shardSections.length, 1, "expected exactly one targeted shard", {
                        explain,
                    });
                    assert.eq(
                        shardSections[0].host,
                        st.rs0.getPrimary().host,
                        "wrong shard targeted",
                        {explain},
                    );
                    assert.eq(shardSections[0].explainVersion, "3", "expected explainVersion '3'", {
                        explain,
                    });
                    assertV3QueryPlanner(getQueryPlanner(shardSections[0]), verbosity, {explain});
                    assertChosenRanker(explain, mode.chosenRanker, mode.reason);
                    assert.eq(
                        aggHasExecutionStats(shardSections[0]),
                        verbosity === kExecutingVerbosity,
                        "unexpected executionStats presence",
                        {explain},
                    );
                    if (verbosity === kExecutingVerbosity) {
                        // The single matching document is also, trivially, the $group's sole
                        // output row.
                        assert.eq(
                            aggExecutionStatsNReturned(shardSections[0]),
                            1,
                            "expected a single group from the single matching document",
                            {explain},
                        );
                    }
                });
            }
        }

        describeUnderEachRankerMode(runShardTargetedTests);
    });

    describe("on a sharded collection with a scatter-gather query", function () {
        function runScatterGatherTests(mode, skipIfCbrUnsupported, skipIfForcedClassicEngine) {
            /**
             * Asserts that every entry of 'plans' carries a costBased node group when 'mode' forced
             * the cost-based ranker and 'verbosity' renders per-node statistics at all - and never
             * otherwise, since only the cost-based ranker computes those estimates.
             */
            function assertCostBasedGroups(plans, verbosity, context) {
                const expectCostBasedGroups = Boolean(
                    mode.forcedConfig &&
                        mode.forcedConfig.internalQueryPlanRanker === "costBased" &&
                        kPlannerStatsVerbosities.includes(verbosity),
                );
                for (const plan of plans) {
                    assert.eq(
                        hasCostBasedGroup(plan),
                        expectCostBasedGroups,
                        "unexpected costBased node group presence",
                        context,
                    );
                }
            }

            for (const verbosity of kV3Verbosities) {
                it(`merges V3 find explains from every shard at ${verbosity}`, function () {
                    if (skipIfCbrUnsupported()) {
                        return;
                    }
                    const explain = assert.commandWorked(
                        db.runCommand({
                            explain: {find: kShardedCollName, filter: multiPlanFilter},
                            verbosity,
                        }),
                    );

                    assert.eq(
                        explain.queryPlanner.winningPlan.stage,
                        "SHARD_MERGE",
                        "a query with no shard-key predicate should scatter",
                        {explain},
                    );

                    const shardSections = getAllNodeExplains(explain);
                    assert.eq(shardSections.length, 2, "expected both shards", {explain});
                    for (const shardSection of shardSections) {
                        assert.eq(shardSection.explainVersion, "3", "expected explainVersion '3'", {
                            explain,
                        });
                        assertV3QueryPlanner(shardSection, verbosity, {explain});
                        assert.eq(
                            shardSection.rankerChoice.chosenRanker,
                            mode.chosenRanker,
                            "expected every shard to agree on the chosen ranker",
                            {explain},
                        );
                        assert.eq(
                            shardSection.rankerChoice.reason,
                            mode.reason,
                            "expected every shard to agree on the reason",
                            {explain},
                        );
                        assertCostBasedGroups(shardSection.plans, verbosity, {explain});
                    }

                    assert.eq(
                        explain.hasOwnProperty("executionStats"),
                        verbosity === kExecutingVerbosity,
                        "unexpected executionStats presence",
                        {explain},
                    );
                    if (verbosity === kExecutingVerbosity) {
                        // Mongos execution stats should include the sum from every shard.
                        assert.eq(
                            explain.executionStats.nReturned,
                            20,
                            "expected the router total to sum every shard's nReturned",
                            {explain},
                        );
                    }
                });

                it(`merges sorted V3 find explains from every shard at ${verbosity}`, function () {
                    if (skipIfCbrUnsupported()) {
                        return;
                    }
                    const explain = assert.commandWorked(
                        db.runCommand({
                            explain: {
                                find: kShardedCollName,
                                filter: multiPlanFilter,
                                sort: {sk: 1},
                            },
                            verbosity,
                        }),
                    );

                    // Note the SHARD_MERGE_SORT stage in mongos vs the non-sorted SHARD_MERGE.
                    assert.eq(
                        explain.queryPlanner.winningPlan.stage,
                        "SHARD_MERGE_SORT",
                        "a scatter-gather query with a sort should merge-sort",
                        {explain},
                    );

                    const shardSections = getAllNodeExplains(explain);
                    assert.eq(shardSections.length, 2, "expected both shards", {explain});
                    for (const shardSection of shardSections) {
                        assert.eq(shardSection.explainVersion, "3", "expected explainVersion '3'", {
                            explain,
                        });
                        assertV3QueryPlanner(shardSection, verbosity, {explain});
                        // Sort is not supported in CBR even if forced.
                        assert.eq(
                            shardSection.rankerChoice.chosenRanker,
                            "multiPlanning",
                            "expected multi-planning to decide regardless of the forced ranker mode",
                            {explain},
                        );
                    }

                    assert.eq(
                        explain.hasOwnProperty("executionStats"),
                        verbosity === kExecutingVerbosity,
                        "unexpected executionStats presence",
                        {explain},
                    );
                    if (verbosity === kExecutingVerbosity) {
                        assert.eq(
                            explain.executionStats.nReturned,
                            20,
                            "expected the router total to sum every shard's nReturned",
                            {explain},
                        );
                    }
                });

                // Test both a merge pipeline that runs on mongos and one that runs on a shard. Per-
                // shard nReturned differs between the two: the plain pipeline lets $group be pushed
                // down and partially computed on each shard (one row per distinct 'a' value, 4+4);
                // $_internalSplitPipeline forces the split point right after $match, so $group runs
                // only on the merging shard and each shard's own part reports its raw match count
                // (10+10, every document, since multiPlanFilter matches everything).
                const mergeCases = [
                    {
                        name: "mongos merge",
                        pipeline: [{$match: multiPlanFilter}, {$group: {_id: "$a", c: {$sum: 1}}}],
                        expectedMergeType: "router",
                        expectedTotalNReturned: 8,
                    },
                    {
                        name: "shard merge",
                        pipeline: [
                            {$match: multiPlanFilter},
                            {$_internalSplitPipeline: {mergeType: "anyShard"}},
                            {$group: {_id: "$a", c: {$sum: 1}}},
                        ],
                        expectedMergeType: "anyShard",
                        expectedTotalNReturned: 20,
                    },
                ];

                for (const {
                    name,
                    pipeline,
                    expectedMergeType,
                    expectedTotalNReturned,
                } of mergeCases) {
                    it(`merges V3 agg explains from every shard on ${name} at ${verbosity}`, function () {
                        if (skipIfCbrUnsupported() || skipIfForcedClassicEngine()) {
                            return;
                        }
                        const explain = assert.commandWorked(
                            db.runCommand({
                                explain: {
                                    aggregate: kShardedCollName,
                                    pipeline,
                                    cursor: {},
                                    allowDiskUse: false,
                                },
                                verbosity,
                            }),
                        );

                        assert.eq(
                            explain.mergeType,
                            expectedMergeType,
                            "the merge ran in the wrong place",
                            {explain},
                        );
                        assert(explain.splitPipeline, "expected a split pipeline", {explain});
                        assert(
                            Array.isArray(explain.splitPipeline.shardsPart),
                            "missing 'shardsPart'",
                            {explain},
                        );
                        assert(
                            Array.isArray(explain.splitPipeline.mergerPart),
                            "missing 'mergerPart'",
                            {explain},
                        );

                        const shardSections = getAllNodeExplains(explain);
                        assert.eq(shardSections.length, 2, "expected both shards", {explain});
                        let totalNReturned = 0;
                        for (const shardSection of shardSections) {
                            assert.eq(
                                shardSection.explainVersion,
                                "3",
                                "expected explainVersion '3'",
                                {explain},
                            );
                            const queryPlanner = getQueryPlanner(shardSection);
                            assertV3QueryPlanner(queryPlanner, verbosity, {explain});
                            assert.eq(
                                queryPlanner.rankerChoice.chosenRanker,
                                mode.chosenRanker,
                                "expected every shard to agree on the chosen ranker",
                                {explain},
                            );
                            if (mode.reason !== undefined) {
                                assert.eq(
                                    queryPlanner.rankerChoice.reason,
                                    mode.reason,
                                    "expected every shard to agree on the reason",
                                    {explain},
                                );
                            }
                            assertCostBasedGroups(queryPlanner.plans, verbosity, {explain});
                            assert.eq(
                                aggHasExecutionStats(shardSection),
                                verbosity === kExecutingVerbosity,
                                "unexpected executionStats presence",
                                {explain},
                            );
                            if (verbosity === kExecutingVerbosity) {
                                totalNReturned += aggExecutionStatsNReturned(shardSection);
                            }
                        }
                        if (verbosity === kExecutingVerbosity) {
                            assert.eq(
                                totalNReturned,
                                expectedTotalNReturned,
                                "unexpected total nReturned summed across shards",
                                {explain},
                            );
                        }
                    });
                }
            }
        }

        describeUnderEachRankerMode(runScatterGatherTests);
    });

    describe("on a query with a single candidate plan", function () {
        // No index covers this predicate, so the only candidate is a collection scan and no ranking takes place.
        const noIndexFilter = {nonexistent: 1};

        function runSingleCandidatePlanTests(mode, skipIfCbrUnsupported) {
            for (const verbosity of kV3Verbosities) {
                it(`reports "singlePlan" on every shard for a scatter find at ${verbosity}`, function () {
                    if (skipIfCbrUnsupported()) {
                        return;
                    }
                    const explain = assert.commandWorked(
                        db.runCommand({
                            explain: {find: kShardedCollName, filter: noIndexFilter},
                            verbosity,
                        }),
                    );

                    const shardSections = getAllNodeExplains(explain);
                    assert.eq(shardSections.length, 2, "expected both shards", {explain});
                    for (const shardSection of shardSections) {
                        assertV3QueryPlanner(shardSection, verbosity, {explain});
                        assert.eq(
                            shardSection.rankerChoice.chosenRanker,
                            "singlePlan",
                            "expected every shard to report a single candidate plan",
                            {explain},
                        );
                        assert.eq(
                            shardSection.plans.length,
                            1,
                            "expected a single candidate plan",
                            {explain},
                        );
                        // CBR will include multiPlanFinalizeStats even for single candidate plans
                        // to determine the numWorks for the plan cache entry.
                        const expectFinalizeStats = Boolean(
                            mode.forcedConfig &&
                                mode.forcedConfig.internalQueryPlanRanker === "costBased" &&
                                kPlannerStatsVerbosities.includes(verbosity),
                        );
                        for (const group of kPlanPhaseGroups) {
                            assert.eq(
                                shardSection.plans[0].hasOwnProperty(group),
                                expectFinalizeStats && group === "multiPlanFinalizeStats",
                                `unexpected '${group}' presence with a single candidate plan`,
                                {explain},
                            );
                        }
                    }

                    assert.eq(
                        explain.hasOwnProperty("executionStats"),
                        verbosity === kExecutingVerbosity,
                        "unexpected executionStats presence",
                        {explain},
                    );
                    if (verbosity === kExecutingVerbosity) {
                        assert.eq(
                            explain.executionStats.nReturned,
                            0,
                            "expected no document to match the nonexistent field",
                            {explain},
                        );
                    }
                });
            }
        }

        describeUnderEachRankerMode(runSingleCandidatePlanTests);
    });
});
