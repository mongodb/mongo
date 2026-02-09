/*
 * Verify that AutomaticCE uses CBR/Multiplanner for $or queries with small number of plans, and the Subplanner otherwise.
 * The current threshold for switching to Subplanner is 16+ plans.
 */
import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {assertPlanNotCosted, getCBRConfig, restoreCBRConfig} from "jstests/libs/query/cbr_utils.js";
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

// Clustered collection.
const clusteredCollName = jsTestName() + "_" + "clustered";
const clusteredColl = db[clusteredCollName];
clusteredColl.drop();
assert.commandWorked(db.createCollection(clusteredCollName, {clusteredIndex: {key: {_id: 1}, unique: true}}));
assert.commandWorked(clusteredColl.insertMany(docs));

const pipeline1 = [
    {
        "$match": {
            "$or": [{"a": {"$lt": 500}, "b": {"$in": [25, 65, 75]}}, {"a": {"$gte": 2000, $lte: 3000}}],
        },
    },
    {"$sort": {"c": 1}},
    {"$skip": 120},
    {"$limit": 400},
];
const pipeline2 = [
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

function testAutomaticUsesNoSubplanner(coll, pipeline, autoStrategy, maxOrSolutions) {
    jsTest.log.info("Running testAutomaticUsesNoSubplanner");
    assert.commandWorked(db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: autoStrategy}));
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryEnumerationMaxOrSolutions: maxOrSolutions}));
    const explain = coll.explain("executionStats").aggregate(pipeline);
    // Total number of query plans should be smaller than the maxOrSolutions.
    assert.lt(explain.queryPlanner.rejectedPlans.length + 1, maxOrSolutions, toJsonForLog(explain));
    const subplan = getPlanStage(explain, "SUBPLAN");
    assert.eq(null, subplan, toJsonForLog(explain));
    assert.eq(explain.executionStats.nReturned, 400, toJsonForLog(explain));
}

function testAutomaticUsesSubplanner(coll, pipeline, autoStrategy, maxOrSolutions) {
    jsTest.log.info("Running testAutomaticUsesSublanner");
    assert.commandWorked(db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: autoStrategy}));
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryEnumerationMaxOrSolutions: maxOrSolutions}));
    const explain = coll.explain("executionStats").aggregate(pipeline);
    const winningPlan = getWinningPlanFromExplain(explain);
    assertPlanNotCosted(winningPlan);
    const subplan = getPlanStage(explain, "SUBPLAN");
    assert.neq(null, subplan, toJsonForLog(explain));
    assert.eq(explain.executionStats.nReturned, 400, toJsonForLog(explain));
}

const maxOrSolutionsLow = 10;
const maxOrSolutionsHigh = 20;
const autoPlanRankingStrategies = ["CBRForNoMultiplanningResults", "CBRCostBasedRankerChoice"];
const allAutoPlanRankingStrategies = [...autoPlanRankingStrategies, "HistogramCEWithHeuristicFallback"];

const prevCBRConfig = getCBRConfig(db);
assert.commandWorked(
    db.adminCommand({setParameter: 1, featureFlagCostBasedRanker: true, internalQueryCBRCEMode: "automaticCE"}),
);

const prevMaxOrSolutions = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalQueryEnumerationMaxOrSolutions: 1}),
).internalQueryEnumerationMaxOrSolutions;

try {
    for (let autoStrategy of autoPlanRankingStrategies) {
        testAutomaticUsesNoSubplanner(coll, pipeline1, autoStrategy, maxOrSolutionsLow);
        testAutomaticUsesSubplanner(coll, pipeline2, autoStrategy, maxOrSolutionsHigh);
    }

    for (let autoStrategy of allAutoPlanRankingStrategies) {
        // All automaticCE strategies use the subplanner for clustered collections.
        // TODO SERVER-117766: Avoid subplanner for $or queries over clustered collections.
        testAutomaticUsesSubplanner(clusteredColl, pipeline1, autoStrategy, maxOrSolutionsLow);
    }

    // The default AutomaticCE strategy always uses the subplanner.
    testAutomaticUsesSubplanner(coll, pipeline1, "HistogramCEWithHeuristicFallback", maxOrSolutionsLow);
    testAutomaticUsesSubplanner(coll, pipeline2, "HistogramCEWithHeuristicFallback", maxOrSolutionsHigh);
} finally {
    restoreCBRConfig(db, prevCBRConfig);
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryEnumerationMaxOrSolutions: prevMaxOrSolutions}),
    );
}
