/*
 * Verify that queries where no results are returned are handled to CBR.
 */
import {getWinningPlanFromExplain, getExecutionStats, getEngine} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted, assertPlanNotCosted} from "jstests/libs/query/cbr_utils.js";

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
    const explain = coll.find({a: {$gte: 1}, b: {$gte: 2}, c: 1}).explain("executionStats");
    const winningPlan = getWinningPlanFromExplain(explain);
    // TODO SERVER-115958: This test fails with featureFlagSbeFull.
    if (getEngine(explain) === "classic") {
        assertPlanCosted(winningPlan);
        // TODO SERVER-115402. Also verify rejected plans when emitted.

        assert.eq(getExecutionStats(explain)[0].nReturned, 0, toJsonForLog(explain));
    }
}

function testResultsQueryIsPlannedWithMultiPlanner() {
    jsTest.log.info("Running testResultsQueryIsPlannedWithMultiPlanner");
    const explain = coll.find({b: 1}).explain("executionStats");
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);
    assert.eq(getExecutionStats(explain)[0].nReturned, 1, toJsonForLog(explain));
}

function testNoResultsQueryWithSinglePlanIsPlannedWithMultiPlanner() {
    jsTest.log.info("Running testNoResultsQueryWithSinglePlanIsPlannedWithMultiPlanner");
    const explain = coll.find({c: 1}).explain("executionStats");
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);
    assert.eq(getExecutionStats(explain)[0].nReturned, 0, toJsonForLog(explain));
}

function testEOFIsPlannedWithMultiPlanner() {
    jsTest.log.info("Running testEOFIsPlannedWithMultiPlanner");
    const explain = coll.find({a: 0, b: 0}).explain("executionStats");
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);

    const executionStats = getExecutionStats(explain)[0];
    assert.eq(executionStats.nReturned, 1, toJsonForLog(explain));
    // TODO SERVER-115958: This test failes with featureFlagSbeFull.
    if (getEngine(explain) === "classic") {
        assert.eq(executionStats.executionStages.works, 2 /* seek + advance */, toJsonForLog(explain));
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
    // TODO SERVER-115402. Also verify rejected plans when emitted.

    assert.eq(getExecutionStats(explain)[0].nReturned, 0, toJsonForLog(explain));

    // Now verify that without the ReturnKey stage, the query is planned with CBR.
    const cbrExplain = coll.find(query).explain("executionStats");
    const cbrWinningPlan = getWinningPlanFromExplain(cbrExplain);
    // TODO SERVER-115958: This test failes with featureFlagSbeFull.
    if (getEngine(cbrExplain) === "classic") {
        assertPlanCosted(cbrWinningPlan);
    }
    assert.eq(getExecutionStats(cbrExplain)[0].nReturned, 0, toJsonForLog(cbrExplain));
}

const prevPlanRankerMode = assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"})).was;
const prevAutoPlanRankingStrategy = assert.commandWorked(
    db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: "CBRForNoMultiplanningResults"}),
).was;
try {
    testNoResultsQueryIsPlannedWithCBR();
    testNoResultsQueryWithSinglePlanIsPlannedWithMultiPlanner();
    testResultsQueryIsPlannedWithMultiPlanner();
    testEOFIsPlannedWithMultiPlanner();
    testReturnKeyIsPlannedWithMultiPlanner();
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: prevPlanRankerMode}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: prevAutoPlanRankingStrategy}),
    );
}
