/*
 * Verify that queries where no results are returned are handled to CBR.
 */
import {
    getWinningPlanFromExplain,
    getExecutionStats,
    getEngine,
    getRejectedPlans,
} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted, assertPlanNotCosted, getCBRConfig, restoreCBRConfig} from "jstests/libs/query/cbr_utils.js";

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
    if (getEngine(explain) === "classic") {
        const winningPlan = getWinningPlanFromExplain(explain);
        assertPlanCosted(winningPlan);
        const rejectedPlans = getRejectedPlans(explain);
        assert.eq(rejectedPlans.length, 3, toJsonForLog(explain));
        assertPlanCosted(rejectedPlans[0]);
        assertPlanNotCosted(rejectedPlans[1]);
        assertPlanNotCosted(rejectedPlans[2]);
        const execStats = getExecutionStats(explain)[0];

        assert.eq(execStats.nReturned, 0, toJsonForLog(explain));

        assert.eq(execStats.allPlansExecution.length, 4, toJsonForLog(explain));
        assertPlanCosted(execStats.allPlansExecution[0].executionStages);
        assert.eq(execStats.allPlansExecution[0].executionStages.advanced, 0, toJsonForLog(explain));
        // TODO SERVER-117670: Revisit how we display the works collected when caching CBR chosen plan.
        // The winning plan chosen by CBR (IXSCAN on "b") will need a total of 9999k works to hit EOF.
        assert.eq(execStats.allPlansExecution[0].executionStages.works, 9999, toJsonForLog(explain));
        assertPlanCosted(execStats.allPlansExecution[1].executionStages);
        assert.eq(execStats.allPlansExecution[1].executionStages.advanced, 0, toJsonForLog(explain));
        assert.eq(execStats.allPlansExecution[1].executionStages.works, 0, toJsonForLog(explain));
        assertPlanNotCosted(execStats.allPlansExecution[2].executionStages);
        assert.eq(execStats.allPlansExecution[2].executionStages.advanced, 0, toJsonForLog(explain));
        assert.eq(execStats.allPlansExecution[2].executionStages.works, 5000, toJsonForLog(explain));
        assertPlanNotCosted(execStats.allPlansExecution[3].executionStages);
        assert.eq(execStats.allPlansExecution[3].executionStages.advanced, 0, toJsonForLog(explain));
        assert.eq(execStats.allPlansExecution[3].executionStages.works, 5000, toJsonForLog(explain));
    }
}

function testResultsQueryIsPlannedWithMultiPlanner() {
    jsTest.log.info("Running testResultsQueryIsPlannedWithMultiPlanner");
    const explain = coll.find({b: 1, a: 1}).explain("allPlansExecution");
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);
    assert.eq(getExecutionStats(explain)[0].nReturned, 1, toJsonForLog(explain));

    const execStats = getExecutionStats(explain)[0];
    assert.eq(execStats.nReturned, 1, toJsonForLog(explain));
    // TODO SERVER-115958: This test failes with featureFlagSbeFull.
    if (getEngine(explain) === "classic") {
        assert.eq(execStats.executionStages.works, 2 /* seek + advance */, toJsonForLog(explain));
        assert.eq(execStats.allPlansExecution.length, 2, toJsonForLog(explain));

        // Winning plan trials' stats
        assertPlanNotCosted(execStats.allPlansExecution[0].executionStages);
        assert.eq(execStats.allPlansExecution[0].executionStages.advanced, 1, toJsonForLog(explain));
        assert.eq(execStats.allPlansExecution[0].executionStages.works, 2, toJsonForLog(explain));

        // Rejected plan trials' stats
        assertPlanNotCosted(execStats.allPlansExecution[1].executionStages);
        assert.eq(execStats.allPlansExecution[1].executionStages.advanced, 1, toJsonForLog(explain));
        assert.eq(execStats.allPlansExecution[1].executionStages.works, 2, toJsonForLog(explain));

        const rejectedPlans = getRejectedPlans(explain);
        assert.eq(rejectedPlans.length, 1, toJsonForLog(explain));
        assertPlanNotCosted(rejectedPlans[0]);
    }
}

function testNoResultsQueryWithSinglePlanDoesNotNeedPlanRanking() {
    jsTest.log.info("Running testNoResultsQueryWithSinglePlanDoesNotNeedPlanRanking");
    const explain = coll.find({c: 1}).explain("allPlansExecution");
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);
    assert.eq(getExecutionStats(explain)[0].nReturned, 0, toJsonForLog(explain));

    const rejectedPlans = getRejectedPlans(explain);
    assert.eq(rejectedPlans.length, 0, toJsonForLog(explain));

    const execStats = getExecutionStats(explain)[0];
    assert.eq(execStats.nReturned, 0, toJsonForLog(explain));
    assert.eq(execStats.allPlansExecution.length, 0, toJsonForLog(explain));
}

function testEOFIsPlannedWithMultiPlanner() {
    jsTest.log.info("Running testEOFIsPlannedWithMultiPlanner");
    const explain = coll.find({a: 0, b: 0}).explain("allPlansExecution");
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);

    const executionStats = getExecutionStats(explain)[0];
    assert.eq(executionStats.nReturned, 1, toJsonForLog(explain));
    // TODO SERVER-115958: This test failes with featureFlagSbeFull.
    if (getEngine(explain) === "classic") {
        assert.eq(executionStats.executionStages.works, 2 /* seek + advance */, toJsonForLog(explain));
        assert.eq(executionStats.allPlansExecution.length, 2, toJsonForLog(explain));

        // Winning plan trials' stats
        assertPlanNotCosted(executionStats.allPlansExecution[0].executionStages);
        assert.eq(executionStats.allPlansExecution[0].executionStages.advanced, 1, toJsonForLog(explain));
        assert.eq(executionStats.allPlansExecution[0].executionStages.works, 2, toJsonForLog(explain));

        // Rejected plan trials' stats
        assertPlanNotCosted(executionStats.allPlansExecution[1].executionStages);
        assert.eq(executionStats.allPlansExecution[1].executionStages.advanced, 1, toJsonForLog(explain));
        assert.eq(executionStats.allPlansExecution[1].executionStages.works, 2, toJsonForLog(explain));

        const rejectedPlans = getRejectedPlans(explain);
        assert.eq(rejectedPlans.length, 1, toJsonForLog(explain));
        assertPlanNotCosted(rejectedPlans[0]);
    }
}

function testReturnKeyIsPlannedWithMultiPlanner() {
    jsTest.log.info("Running testReturnKeyIsPlannedWithMultiPlanner");
    const query = {a: {$gte: 1}, b: {$gte: 2}, c: 1};
    // The ReturnKey stage is currently unsupported by the cardinality estimator, so we expect
    // the planner to fall back to the legacy multi-planner.
    const explain = coll.find(query).returnKey().explain("executionStats");
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);
    assert.eq(getExecutionStats(explain)[0].nReturned, 0, toJsonForLog(explain));

    if (getEngine(explain) === "classic") {
        const rejectedPlans = getRejectedPlans(explain);
        assert.eq(rejectedPlans.length, 3, toJsonForLog(explain));
        assertPlanNotCosted(rejectedPlans[0]);
        // CBR plans are not costed since the returnKey stage is not costable.
        assertPlanNotCosted(rejectedPlans[1]);
        assertPlanNotCosted(rejectedPlans[2]);
    }

    // Now verify that without the ReturnKey stage, the query is planned with CBR.
    const cbrExplain = coll.find(query).explain("executionStats");
    const cbrWinningPlan = getWinningPlanFromExplain(cbrExplain);
    // TODO SERVER-115958: This test failes with featureFlagSbeFull.
    if (getEngine(cbrExplain) === "classic") {
        assertPlanCosted(cbrWinningPlan);

        const rejectedPlans = getRejectedPlans(cbrExplain);
        assert.eq(rejectedPlans.length, 3, toJsonForLog(cbrExplain));
        assertPlanCosted(rejectedPlans[0]);
        assertPlanNotCosted(rejectedPlans[1]);
        assertPlanNotCosted(rejectedPlans[2]);
    }
    assert.eq(getExecutionStats(cbrExplain)[0].nReturned, 0, toJsonForLog(cbrExplain));
}

const prevCBRConfig = getCBRConfig(db);

assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        featureFlagCostBasedRanker: true,
        internalQueryCBRCEMode: "automaticCE",
        automaticCEPlanRankingStrategy: "CBRForNoMultiplanningResults",
    }),
);

const execYieldIterations = db.adminCommand({
    getParameter: 1,
    internalQueryExecYieldIterations: 1,
}).internalQueryExecYieldIterations;

// Deterministic sample generation to ensure plan selection stability.
const prevSequentialSamplingScan = assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}),
).was;
try {
    testNoResultsQueryIsPlannedWithCBR();
    testNoResultsQueryWithSinglePlanDoesNotNeedPlanRanking();
    testResultsQueryIsPlannedWithMultiPlanner();
    testEOFIsPlannedWithMultiPlanner();
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
    testReturnKeyIsPlannedWithMultiPlanner();
} finally {
    restoreCBRConfig(db, prevCBRConfig);

    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryExecYieldIterations: execYieldIterations,
        }),
    );
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: prevSequentialSamplingScan}),
    );
}
