/*
 * Verify that AutomaticCE uses CBR/Multiplanner for $or queries with small number of plans, and the Subplanner otherwise.
 * The current threshold for switching to Subplanner is 16+ plans.
 */
import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted, assertPlanNotCosted} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTest.log.info(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const docs = [];
for (let i = 0; i < 10000; i++) {
    docs.push({a: i, b: i, c: i % 77});
}
assert.commandWorked(coll.insertMany(docs));

assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}, {c: 1}, {a: 1, b: 1}]));

const pipeline = [
    {
        "$match": {
            "$or": [
                {"a": {"$lt": 500}, "b": {"$in": [25, 65, 75]}},
                {"a": {"$gte": 2000, $lte: 3000}, "b": {"$gte": 2450}},
                {"a": {"$gte": 8000}, "b": {"$in": [8080, 9120]}},
            ],
        },
    },
    {"$sort": {"c": 1}},
    {"$skip": 120},
    {"$limit": 400},
];

function testAutomaticUsesNoSubplanner(maxEnumeratedPlans) {
    jsTest.log.info("Running testAutomaticUsesNoSubplanner");
    const explain = coll.explain("executionStats").aggregate(pipeline);
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanCosted(winningPlan);
    assert.eq(explain.queryPlanner.rejectedPlans.length, maxEnumeratedPlans - 1, toJsonForLog(explain)); // -1 for the winning plan
    const subplan = getPlanStage(explain, "SUBPLAN");
    assert.eq(null, subplan, toJsonForLog(explain));
    assert.eq(explain.executionStats.nReturned, 400, toJsonForLog(explain));
}

function testAutomaticUsesSubplanner() {
    jsTest.log.info("Running testAutomaticUsesSublanner");
    const explain = coll.explain("executionStats").aggregate(pipeline);
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);
    const subplan = getPlanStage(explain, "SUBPLAN");
    assert.neq(null, subplan, toJsonForLog(explain));
    assert.eq(explain.executionStats.nReturned, 400, toJsonForLog(explain));
}

const prevPlanRankerMode = assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"})).was;

const maxOrSolutionsLow = 10;
const maxOrSolutionsHigh = 20;
const prevMaxOrSolutions = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalQueryEnumerationMaxOrSolutions: 1}),
).internalQueryEnumerationMaxOrSolutions;

const autoPlanRankingStrategies = ["CBRForNoMultiplanningResults", "CBRCostBasedRankerChoice"];
const prevAutoStrategy = assert.commandWorked(
    db.adminCommand({getParameter: 1, automaticCEPlanRankingStrategy: 1}),
).automaticCEPlanRankingStrategy;

try {
    for (let autoStrategy of autoPlanRankingStrategies) {
        assert.commandWorked(db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: autoStrategy}));

        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryEnumerationMaxOrSolutions: maxOrSolutionsLow}),
        );
        if (autoStrategy === "CBRCostBasedRankerChoice") {
            // TODO SERVER-117372. Explain multiplanner plans when cost based choice is active.
            testAutomaticUsesNoSubplanner(maxOrSolutionsLow + 1);
        } else {
            testAutomaticUsesNoSubplanner(2 * (maxOrSolutionsLow + 1)); // Every strategy (MP + CBR) generates maxOrSolutionsLow plans plus one since there's an index that could be used to satisfy the sorting.
        }

        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryEnumerationMaxOrSolutions: maxOrSolutionsHigh}),
        );
        testAutomaticUsesSubplanner();
    }

    // The default AutomaticCE strategy always uses the subplanner.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: "HistogramCEWithHeuristicFallback"}),
    );
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryEnumerationMaxOrSolutions: maxOrSolutionsLow}));
    testAutomaticUsesSubplanner();
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: prevPlanRankerMode}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryEnumerationMaxOrSolutions: prevMaxOrSolutions}),
    );
    assert.commandWorked(db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: prevAutoStrategy}));
}
