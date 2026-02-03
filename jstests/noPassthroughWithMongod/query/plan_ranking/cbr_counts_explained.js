// Tests that count queries have their winning and rejected plans wrapped in CountStages.

import {
    assertExplainCount,
    getRejectedPlan,
    getRejectedPlans,
    getWinningPlanFromExplain,
    isIxscan,
    isCountScan,
} from "jstests/libs/query/analyze_plan.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const docs = [];
for (let i = 0; i < 10000; i++) {
    docs.push({a: i, b: i});
}
assert.commandWorked(coll.insertMany(docs));

assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}, {a: 1, b: 1}]));

function runTestIxscan() {
    const explain = coll
        .explain("allPlansExecution")
        .find({a: {$gte: 1}, b: {$gte: 2}, c: 1})
        .count();

    assertExplainCount({explainResults: explain, expectedCount: 0});
    assert(isIxscan(db, getWinningPlanFromExplain(explain)), {explain});

    const rejectedPlans = getRejectedPlans(explain);
    assert(rejectedPlans.length >= 1, {explain});
    for (let curRejectedPlan of rejectedPlans) {
        const rejectedPlan = getRejectedPlan(curRejectedPlan);
        assert.eq(rejectedPlan.stage, "COUNT", {rejectedPlan, explain});
        assert(isIxscan(db, rejectedPlan), {rejectedPlan, explain});
    }
}

function runTestFastScan() {
    const explain = coll
        .explain("allPlansExecution")
        .find({a: {$gte: 0}})
        .count();

    assertExplainCount({explainResults: explain, expectedCount: 10000});
    assert(isCountScan(db, getWinningPlanFromExplain(explain)), {explain});

    const rejectedPlans = getRejectedPlans(explain);
    // We throw away all other plans if we have a fast count.
    assert(rejectedPlans.length == 0, {explain});
}

const prevPlanRankerMode = assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"})).was;
const prevAutoPlanRankingStrategy = assert.commandWorked(
    assert.commandWorked(db.adminCommand({getParameter: 1, automaticCEPlanRankingStrategy: 1})),
).automaticCEPlanRankingStrategy;
try {
    const cbrFallbackStrategies = ["CBRForNoMultiplanningResults", "CBRCostBasedRankerChoice"];

    for (const cbrFallbackStrategy of cbrFallbackStrategies) {
        jsTest.log.info("Running with:", {cbrFallbackStrategy});
        assert.commandWorked(db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: cbrFallbackStrategy}));
        runTestIxscan();
        runTestFastScan();
    }
} finally {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            planRankerMode: prevPlanRankerMode,
            automaticCEPlanRankingStrategy: prevAutoPlanRankingStrategy,
        }),
    );
}
