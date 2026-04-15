/**
 * Verify that queries where no results are returned are handled by CBR, and that queries
 * with results or EOF are handled by the multi-planner.
 *
 * On sharded topologies, SHARDING_FILTER is inestimable by CBR's sampling estimator, so CBR
 * never triggers. The test still runs to exercise the planning flow; strict costed/not-costed
 * assertions are only enforced on non-sharded topologies.
 *
 * @tags: [
 *    # setParameter calls to enable CBR will fail if a stepdown happens in between.
 *    does_not_support_stepdowns,
 *    # Timeseries bucket collections don't have user-created indexes, leading to COLLSCAN.
 *    exclude_from_timeseries_crud_passthrough,
 *    # featureFlagCostBasedRanker was introduced in 9.0.
 *    requires_fcv_90,
 *    # setParameter calls to enable CBR will hang on nodes having an initial sync and cause timeouts.
 *    incompatible_with_initial_sync,
 *    # Explain calls will fail if a migration is going on.
 *    assumes_balancer_off,
 * ]
 */
import {
    getWinningPlanFromExplain,
    getEngine,
    getRejectedPlans,
    getPlanStage,
    getPlanStages,
} from "jstests/libs/query/analyze_plan.js";
import {
    assertPlanCosted,
    assertPlanNotCosted,
    isPlanCosted,
    getCBRConfig,
    setCBRConfigOnAllNonConfigNodes,
} from "jstests/libs/query/cbr_utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 * Runs setParameter on all mongod nodes in the fixture.
 */
function setParameterOnAllNodes(db, params) {
    FixtureHelpers.mapOnEachShardNode({
        db,
        func: (nodeDb) => assert.commandWorked(nodeDb.adminCommand(Object.assign({setParameter: 1}, params))),
        primaryNodeOnly: false,
    });
}

/**
 * Returns a database connection to a mongod (not mongos) for getParameter calls.
 */
function getMongodDb(db) {
    if (FixtureHelpers.isMongos(db)) {
        return FixtureHelpers.getPrimaries(db)[0].getDB(db.getName());
    }
    return db;
}

function isExecutionSplitInShards(explain) {
    return FixtureHelpers.isMongos(db) && explain?.queryPlanner?.winningPlan?.stage !== "SINGLE_SHARD";
}

function shardsHaveShardingFilter(explain) {
    return getPlanStages(explain, "SHARDING_FILTER").length > 0;
}

function hasV1Index(winningPlan) {
    return getPlanStage(winningPlan, "IXSCAN")?.indexVersion === 1;
}

/**
 * Returns the number of shards that participated in the query from the explain output.
 */
function getNumShardsFromExplain(explain) {
    return explain?.queryPlanner?.winningPlan?.shards?.length ?? 1;
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const docs = [];
for (let i = 0; i < 10000; i++) {
    docs.push({a: i, b: i});
}
assert.commandWorked(coll.insertMany(docs));

assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

function testNoResultsQueryIsPlannedWithCBR() {
    jsTest.log.info("Running testNoResultsQueryIsPlannedWithCBR");
    const explain = coll.find({a: {$gte: 1}, b: {$gte: 2}, c: 1}).explain("allPlansExecution");
    // TODO SERVER-115958: This test fails with featureFlagSbeFull.
    if (getEngine(explain) !== "classic") {
        return;
    }
    const winningPlan = getWinningPlanFromExplain(explain);
    const rejectedPlans = getRejectedPlans(explain);
    if (isExecutionSplitInShards(explain)) {
        const numShards = getNumShardsFromExplain(explain);
        if (shardsHaveShardingFilter(explain)) {
            // SHARDING_FILTER is inestimable by CBR's sampling estimator, so CBR cannot pick a
            // single winner. All plans end up not costed. At least 1 rejected per shard.
            assertPlanNotCosted(winningPlan);
            assert.gte(rejectedPlans.length, numShards, toJsonForLog(explain));
            for (const plan of rejectedPlans) {
                assertPlanNotCosted(plan);
            }
        } else {
            assertPlanCosted(winningPlan);
            // Each shard contributes 1 not-costed (MP) + 1 costed (CBR) rejected plan.
            assert.eq(rejectedPlans.length, 2 * numShards, toJsonForLog(explain));
            let numCosted = 0;
            for (const plan of rejectedPlans) {
                if (isPlanCosted(plan)) {
                    assertPlanCosted(plan);
                    numCosted++;
                } else {
                    assertPlanNotCosted(plan);
                }
            }
            assert.eq(numCosted, numShards, toJsonForLog(explain));
        }
    } else if (winningPlan.stage === "SHARDING_FILTER") {
        // Shard-local but SHARDING_FILTER still present: 1 from MP + 2 from CBR, all not costed.
        assertPlanNotCosted(winningPlan);
        assert.eq(rejectedPlans.length, 3, toJsonForLog(explain));
        for (const plan of rejectedPlans) {
            assertPlanNotCosted(plan);
        }
    } else if (hasV1Index(winningPlan)) {
        // v1 indexes are inestimable by CBR: 1 from MP + 2 from CBR, all not costed.
        assertPlanNotCosted(winningPlan);
        assert.eq(rejectedPlans.length, 3, toJsonForLog(explain));
        for (const plan of rejectedPlans) {
            assertPlanNotCosted(plan);
        }
    } else {
        // CBR chose the winning plan — it should be costed.
        assertPlanCosted(winningPlan);
        // 2 rejected plans: 1 from MP (not costed) + 1 from CBR (costed).
        assert.eq(rejectedPlans.length, 2, toJsonForLog(explain));
        assertPlanNotCosted(rejectedPlans[0]);
        assertPlanCosted(rejectedPlans[1]);
    }
}

function testResultsQueryIsPlannedWithMultiPlanner() {
    jsTest.log.info("Running testResultsQueryIsPlannedWithMultiPlanner");
    const explain = coll.find({b: 1, a: 1}).explain("allPlansExecution");
    if (getEngine(explain) !== "classic") {
        return;
    }
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);
    // MP found results, so CBR was not invoked. 1 rejected plan per shard (the MP loser).
    const rejectedPlans = getRejectedPlans(explain);
    if (isExecutionSplitInShards(explain)) {
        const numShards = getNumShardsFromExplain(explain);
        assert.eq(rejectedPlans.length, numShards, toJsonForLog(explain));
    } else {
        assert.eq(rejectedPlans.length, 1, toJsonForLog(explain));
    }
    for (const plan of rejectedPlans) {
        assertPlanNotCosted(plan);
    }
}

function testNoResultsQueryWithSinglePlanDoesNotNeedPlanRanking() {
    jsTest.log.info("Running testNoResultsQueryWithSinglePlanDoesNotNeedPlanRanking");
    const explain = coll.find({c: 1}).explain("allPlansExecution");
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);
    const rejectedPlans = getRejectedPlans(explain);
    assert.eq(rejectedPlans.length, 0, toJsonForLog(explain));
}

function testEOFIsPlannedWithMultiPlanner() {
    jsTest.log.info("Running testEOFIsPlannedWithMultiPlanner");
    const explain = coll.find({a: 0, b: 0}).explain("allPlansExecution");
    if (getEngine(explain) !== "classic") {
        return;
    }
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);
    // MP early-exited (EOF). 1 rejected plan per shard (the MP loser).
    const rejectedPlans = getRejectedPlans(explain);
    if (isExecutionSplitInShards(explain)) {
        const numShards = getNumShardsFromExplain(explain);
        assert.eq(rejectedPlans.length, numShards, toJsonForLog(explain));
    } else {
        assert.eq(rejectedPlans.length, 1, toJsonForLog(explain));
    }
    for (const plan of rejectedPlans) {
        assertPlanNotCosted(plan);
    }
}

function testReturnKeyIsPlannedWithMultiPlanner() {
    jsTest.log.info("Running testReturnKeyIsPlannedWithMultiPlanner");
    const query = {a: {$gte: 1}, b: {$gte: 2}, c: 1};

    // With ReturnKey: all plans inestimable.
    {
        const explain = coll.find(query).returnKey().explain("executionStats");
        if (getEngine(explain) !== "classic") {
            return;
        }
        assertPlanNotCosted(getWinningPlanFromExplain(explain));
        const rejectedPlans = getRejectedPlans(explain);
        if (isExecutionSplitInShards(explain)) {
            const numShards = getNumShardsFromExplain(explain);
            if (shardsHaveShardingFilter(explain)) {
                // ReturnKey + SHARDING_FILTER: all inestimable. At least 1 rejected per shard.
                assert.gte(rejectedPlans.length, numShards, toJsonForLog(explain));
            } else {
                // ReturnKey inestimable: numShards MP losers + at least 2 from CBR.
                assert.gte(rejectedPlans.length, numShards + 2, toJsonForLog(explain));
            }
            // ReturnKey makes all plans inestimable regardless of SHARDING_FILTER.
            for (const plan of rejectedPlans) {
                assertPlanNotCosted(plan);
            }
        } else {
            // 3 rejected: 1 from MP + 2 from CBR (none estimable due to return key).
            assert.eq(rejectedPlans.length, 3, toJsonForLog(explain));
            for (const plan of rejectedPlans) {
                assertPlanNotCosted(plan);
            }
        }
    }

    // Without ReturnKey: same assertions as testNoResultsQueryIsPlannedWithCBR.
    {
        const explain = coll.find(query).explain("executionStats");
        if (getEngine(explain) !== "classic") {
            return;
        }
        const winningPlan = getWinningPlanFromExplain(explain);
        const rejectedPlans = getRejectedPlans(explain);
        if (isExecutionSplitInShards(explain)) {
            const numShards = getNumShardsFromExplain(explain);
            if (shardsHaveShardingFilter(explain)) {
                assertPlanNotCosted(winningPlan);
                assert.gte(rejectedPlans.length, numShards, toJsonForLog(explain));
                for (const plan of rejectedPlans) {
                    assertPlanNotCosted(plan);
                }
            } else {
                assertPlanCosted(winningPlan);
                // Each shard contributes 1 not-costed (MP) + 1 costed (CBR) rejected plan.
                assert.eq(rejectedPlans.length, 2 * numShards, toJsonForLog(explain));
                let numCosted = 0;
                for (const plan of rejectedPlans) {
                    if (isPlanCosted(plan)) {
                        assertPlanCosted(plan);
                        numCosted++;
                    } else {
                        assertPlanNotCosted(plan);
                    }
                }
                assert.eq(numCosted, numShards, toJsonForLog(explain));
            }
        } else if (winningPlan.stage === "SHARDING_FILTER") {
            // Shard-local but SHARDING_FILTER still present: 1 from MP + 2 from CBR, all not
            // costed.
            assertPlanNotCosted(winningPlan);
            assert.eq(rejectedPlans.length, 3, toJsonForLog(explain));
            for (const plan of rejectedPlans) {
                assertPlanNotCosted(plan);
            }
        } else if (hasV1Index(winningPlan)) {
            // v1 indexes are inestimable by CBR: 1 from MP + 2 from CBR, all not costed.
            assertPlanNotCosted(winningPlan);
            assert.eq(rejectedPlans.length, 3, toJsonForLog(explain));
            for (const plan of rejectedPlans) {
                assertPlanNotCosted(plan);
            }
        } else {
            // CBR chose the winning plan — it should be costed.
            assertPlanCosted(winningPlan);
            // 2 rejected plans: 1 from MP (not costed) + 1 from CBR (costed).
            assert.eq(rejectedPlans.length, 2, toJsonForLog(explain));
            assertPlanNotCosted(rejectedPlans[0]);
            assertPlanCosted(rejectedPlans[1]);
        }
    }
}

const mongodDb = getMongodDb(db);
const prevCBRConfig = getCBRConfig(mongodDb);

setCBRConfigOnAllNonConfigNodes(db.getMongo(), {
    featureFlagCostBasedRanker: true,
    internalQueryCBRCEMode: "automaticCE",
    automaticCEPlanRankingStrategy: "CBRForNoMultiplanningResults",
});

// Deterministic sample generation to ensure plan selection stability.
const prevSequentialSamplingScan = assert.commandWorked(
    mongodDb.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}),
).was;
setParameterOnAllNodes(db, {internalQuerySamplingBySequentialScan: true});

// Force frequent yields to exercise yield/restore.
const prevExecYieldIterations = assert.commandWorked(
    mongodDb.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}),
).was;
setParameterOnAllNodes(db, {internalQueryExecYieldIterations: 1});

try {
    testNoResultsQueryIsPlannedWithCBR();
    testNoResultsQueryWithSinglePlanDoesNotNeedPlanRanking();
    testResultsQueryIsPlannedWithMultiPlanner();
    testEOFIsPlannedWithMultiPlanner();
    testReturnKeyIsPlannedWithMultiPlanner();
} finally {
    setCBRConfigOnAllNonConfigNodes(db.getMongo(), prevCBRConfig);
    setParameterOnAllNodes(db, {
        internalQuerySamplingBySequentialScan: prevSequentialSamplingScan,
        internalQueryExecYieldIterations: prevExecYieldIterations,
    });
}
