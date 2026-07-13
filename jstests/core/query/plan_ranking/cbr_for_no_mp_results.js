/**
 * Verify that queries where no results are returned are handled by CBR, and that queries
 * with results or EOF are handled by the multi-planner.
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
 *    # The test uses explain("allPlansExecution") with samplingCE and asserts on the exact number
 *    # of costed vs. non-costed rejected plans per shard. In causally consistent sessions the
 *    # session state interacts with how samplingCE estimates are propagated across shards,
 *    # causing the explain output to differ from the expected plan structure.
 *    does_not_support_causal_consistency,
 * ]
 */

import {
    getWinningPlanFromExplain,
    getEngine,
    getRejectedPlans,
    getPlanStage,
    assertChosenRanker,
    ChosenRanker,
    PlanRankerReason,
} from "jstests/libs/query/analyze_plan.js";
import {
    assertPlanCosted,
    assertPlanNotCosted,
    isPlanCosted,
    getPlanRankerConfig,
    setPlanRankerConfigOnAllNonConfigNodes,
} from "jstests/libs/query/cbr_utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 * Runs setParameter on all mongod nodes in the fixture.
 */
function setParameterOnAllNodes(db, params) {
    FixtureHelpers.mapOnEachShardNode({
        db,
        func: (nodeDb) =>
            assert.commandWorked(nodeDb.adminCommand(Object.assign({setParameter: 1}, params))),
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
    return (
        FixtureHelpers.isMongos(db) && explain?.queryPlanner?.winningPlan?.stage !== "SINGLE_SHARD"
    );
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

// The multi-planner works each candidate plan up to a per-plan trial budget of
// max(internalQueryPlanEvaluationWorks, internalQueryPlanEvaluationCollFraction * numRecords)
// (the coll fraction is 0.3 for these two-plan queries). For NoMultiplanningResults to
// engage on the no-results query below, no candidate plan may hit EOF (or return results) within
// that budget on any single shard. If a plan finishes first, earlyExit is set to true and CBR is
// not considered.
//
// We keep internalQueryPlanEvaluationWorks small (see kPlanEvaluationWorks below) so the budget
// floor is low, then size the collection so that the number of matching index entries per shard
// comfortably exceeds the budget even when the fixture spreads the data across many shards. With
// docs-per-shard >> max(kPlanEvaluationWorks, 0.3 * docs-per-shard) the plans never exhaust their
// index scan during the trial. 2000 docs keeps this true down to ~10 shards while being 50x
// cheaper to load than the previous 100000.
const kPlanEvaluationWorks = 100;
const docs = [];
for (let i = 0; i < 2000; i++) {
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
    // MP produced no results within its trial budget, so CBR was engaged to rank the plans.
    if (isExecutionSplitInShards(explain)) {
        assertChosenRanker(explain, {
            chosenRanker: ChosenRanker.kCostBased,
            reason: PlanRankerReason.kNoMultiplanningResults,
        });
        const numShards = getNumShardsFromExplain(explain);
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
    } else if (hasV1Index(winningPlan)) {
        // v1 indexes are inestimable by CBR, so it fell back to MP: 1 from MP + 2 from CBR, all
        // not costed.
        assertChosenRanker(explain, {
            chosenRanker: ChosenRanker.kMultiPlanning,
            reason: PlanRankerReason.kNoMultiplanningResults,
        });
        assert.eq(rejectedPlans.length, 3, toJsonForLog(explain));
        for (const plan of rejectedPlans) {
            assertPlanNotCosted(plan);
        }
    } else {
        // CBR chose the winning plan — it should be costed.
        assertChosenRanker(explain, {
            chosenRanker: ChosenRanker.kCostBased,
            reason: PlanRankerReason.kNoMultiplanningResults,
        });
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
    // MP found results, so CBR was not invoked and MP picked the winner.
    assertChosenRanker(explain, {
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kMpEarlyExitOrResult,
    });
    // 1 rejected plan per shard (the MP loser).
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
    // Only one candidate plan, so no ranking was needed.
    assertChosenRanker(explain, {
        chosenRanker: ChosenRanker.kNone,
        reason: PlanRankerReason.kSinglePlan,
    });
    const rejectedPlans = getRejectedPlans(explain);
    assert.eq(rejectedPlans.length, 0, toJsonForLog(explain));
}

function testEOFIsPlannedWithMultiPlanner() {
    jsTest.log.info("Running testEOFIsPlannedWithMultiPlanner");
    const explain = coll.find({a: 0, b: 0}).explain("allPlansExecution");
    if (getEngine(explain) !== "classic") {
        return;
    }
    // MP early-exited (EOF), so it picked the winner without engaging CBR.
    assertChosenRanker(explain, {
        chosenRanker: ChosenRanker.kMultiPlanning,
        reason: PlanRankerReason.kMpEarlyExitOrResult,
    });
    // 1 rejected plan per shard (the MP loser).
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
        // No MP results, so CBR was engaged, but every plan is inestimable due to RETURN_KEY, so
        // CBR fell back to MP.
        assertChosenRanker(explain, {
            chosenRanker: ChosenRanker.kMultiPlanning,
            reason: PlanRankerReason.kInestimableNode,
        });
        const rejectedPlans = getRejectedPlans(explain);
        if (isExecutionSplitInShards(explain)) {
            const numShards = getNumShardsFromExplain(explain);
            // numShards MP losers + at least 2 from CBR (all not costed due to RETURN_KEY).
            assert.gte(rejectedPlans.length, numShards + 2, toJsonForLog(explain));
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
        // MP produced no results within its trial budget, so CBR was engaged to rank the plans.
        if (isExecutionSplitInShards(explain)) {
            assertChosenRanker(explain, {
                chosenRanker: ChosenRanker.kCostBased,
                reason: PlanRankerReason.kNoMultiplanningResults,
            });
            const numShards = getNumShardsFromExplain(explain);
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
        } else if (hasV1Index(winningPlan)) {
            // v1 indexes are inestimable by CBR, so it fell back to MP: 1 from MP + 2 from CBR,
            // all not costed.
            assertChosenRanker(explain, {
                chosenRanker: ChosenRanker.kMultiPlanning,
                reason: PlanRankerReason.kNoMultiplanningResults,
            });
            assert.eq(rejectedPlans.length, 3, toJsonForLog(explain));
            for (const plan of rejectedPlans) {
                assertPlanNotCosted(plan);
            }
        } else {
            // CBR chose the winning plan — it should be costed.
            assertChosenRanker(explain, {
                chosenRanker: ChosenRanker.kCostBased,
                reason: PlanRankerReason.kNoMultiplanningResults,
            });
            assertPlanCosted(winningPlan);
            // 2 rejected plans: 1 from MP (not costed) + 1 from CBR (costed).
            assert.eq(rejectedPlans.length, 2, toJsonForLog(explain));
            assertPlanNotCosted(rejectedPlans[0]);
            assertPlanCosted(rejectedPlans[1]);
        }
    }
}

const mongodDb = getMongodDb(db);
// TODO: SERVER-130178 Fix CBR-test utils to work in sharded environment.
// This utility function assumes direct communication to mongod. Check and extend it to work in sharded environment.
// Check if the tag does_not_support_causal_consistency is still needed after SERVER-130178 is fixed.
const prevPlanRankerConfig = getPlanRankerConfig(mongodDb);

setPlanRankerConfigOnAllNonConfigNodes(db.getMongo(), {
    featureFlagCostBasedRanker: true,
    internalQueryPlanRanker: "mixed",
    internalQueryCBRCEMode: "samplingCE",
    internalQueryMixedPlanRankingStrategy: "NoMultiplanningResults",
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

// Keep the per-plan trial budget floor low so the collection above can stay small while still
// preventing any plan from hitting EOF during the trial (see the comment on 'docs').
const prevPlanEvaluationWorks = assert.commandWorked(
    mongodDb.adminCommand({
        setParameter: 1,
        internalQueryPlanEvaluationWorks: kPlanEvaluationWorks,
    }),
).was;
setParameterOnAllNodes(db, {internalQueryPlanEvaluationWorks: kPlanEvaluationWorks});

try {
    testNoResultsQueryIsPlannedWithCBR();
    testNoResultsQueryWithSinglePlanDoesNotNeedPlanRanking();
    testResultsQueryIsPlannedWithMultiPlanner();
    testEOFIsPlannedWithMultiPlanner();
    testReturnKeyIsPlannedWithMultiPlanner();
    // TODO SERVER-115714 For posterity add a test case for small collections that use MP only.
} finally {
    setPlanRankerConfigOnAllNonConfigNodes(db.getMongo(), prevPlanRankerConfig);
    setParameterOnAllNodes(db, {
        internalQuerySamplingBySequentialScan: prevSequentialSamplingScan,
        internalQueryExecYieldIterations: prevExecYieldIterations,
        internalQueryPlanEvaluationWorks: prevPlanEvaluationWorks,
    });
}
