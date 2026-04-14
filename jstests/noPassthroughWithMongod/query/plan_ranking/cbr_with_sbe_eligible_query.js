import {resultsEq} from "jstests/aggregation/extras/utils.js";
import {checkSbeStatus, kSbeDisabled, isDeferredGetExecutorEnabled} from "jstests/libs/query/sbe_util.js";
import {getCBRConfig, setCBRConfig, isPlanCosted} from "jstests/libs/query/cbr_utils.js";
import {getEngine} from "jstests/libs/query/analyze_plan.js";

// TODO SERVER-117707 after this ticket, we should always expect that CBR is supported for SBE-targeted queries.
const cbrSupportedForSbeQueries = isDeferredGetExecutorEnabled(db);

const collName = jsTestName();
const coll = db[collName];
coll.drop();
coll.insert({_id: 0, a: 0});

// Set up a collection where a query against it could fallback to CBR.
const cbrQueryCollName = collName + "_cbr";
const cbrQueryColl = db[cbrQueryCollName];
cbrQueryColl.drop();
const docs = [];
for (let i = 0; i < 20000; i++) {
    docs.push({_id: i, a: i, b: i});
}
docs.push({_id: 20000, a: 20000, b: 20000, c: 1});
cbrQueryColl.insertMany(docs);
cbrQueryColl.createIndex({a: 1});
cbrQueryColl.createIndex({b: 1});

function sbeEligibleGroupDistinctScanTest() {
    jsTest.log.info("Test Case: sbeEligibleGroupDistinctScanTest");
    const pipeline = [{$match: {a: 0}}, {$group: {_id: "$_id"}}];
    const results = coll.aggregate(pipeline).toArray();
    assert(resultsEq(results, [{_id: 0}]));
}

function assertSbeWithCbrFallbackBehavior() {
    // Verify that SBE-eligible queries interact correctly with CBR. When CBR is enabled for SBE
    // queries, we expect costed rejected plans; when it is not, the query should be planned via
    // standard multiplanning without CBR influence.
    // TODO SERVER-117707: Modify/delete this test.
    jsTest.log.info("Test Case: assertSbeWithCbrFallbackBehavior");
    // The assertions below only make sense when the queries are eligible for SBE, i.e.
    // when the query framework control is trySbeEngine or higher.
    if (checkSbeStatus(db) == kSbeDisabled) {
        return;
    }

    const config = getCBRConfig(db);
    const cbrEnabled = config.featureFlagCostBasedRanker;
    const mpWithCbrFallbackEnabled =
        cbrEnabled && config.automaticCEPlanRankingStrategy === "CBRForNoMultiplanningResults";

    const pipelines = [
        // Queries that return results during the multiplanning trial period (no CBR fallback).
        {queryReturnsNoResults: false, pipeline: [{$match: {a: 20000, b: 20000, c: 1}}, {$group: {_id: "$a"}}]}, // distinct
        {queryReturnsNoResults: false, pipeline: [{$match: {a: 20000, b: 20000, c: 1}}, {$group: {_id: null}}]}, // non-distinct
        // Queries that return no results during the multiplanning trial period (may trigger CBR fallback).
        {queryReturnsNoResults: true, pipeline: [{$match: {a: {$gte: 1}, b: {$gte: 1}, c: 1}}, {$group: {_id: "$a"}}]}, // distinct
        {queryReturnsNoResults: true, pipeline: [{$match: {a: {$gte: 1}, b: {$gte: 1}, c: 1}}, {$group: {_id: null}}]}, // non-distinct
    ];

    for (const {queryReturnsNoResults, pipeline} of pipelines) {
        jsTest.log.info("Pipeline: " + tojson(pipeline));
        const explain = cbrQueryColl.explain().aggregate(pipeline);
        assert.eq(getEngine(explain), "sbe", explain);

        const rejectedPlans = explain.queryPlanner.rejectedPlans;
        const queryShouldUseCbr = cbrEnabled && cbrSupportedForSbeQueries && queryReturnsNoResults;
        if (queryShouldUseCbr) {
            // If CBR is used, at least one of the rejected plans should have costs. This doesn't apply to
            // all rejected plans because when MP with CBR fallback is enabled, half of the plans will have
            // costs and the other half won't.
            assert(
                rejectedPlans.some((rejectedPlan) => isPlanCosted(rejectedPlan)),
                rejectedPlans,
            );
        }

        // If MP with CBR fallback is enabled for SBE-targeted queries, we should see 2 plans. Otherwise, we should just see 1.
        const expectedNumPlans = mpWithCbrFallbackEnabled && queryShouldUseCbr ? 2 : 1;
        assert.eq(rejectedPlans.length, expectedNumPlans, tojson(explain));

        // Run the query 3 times to ensure that when we extract the plan from the plan cache we do not error.
        for (let i = 0; i < 3; i++) {
            assert.eq(
                cbrQueryColl.aggregate(pipeline).toArray().length,
                1,
                "Expected 1 result, results are: " + tojson(cbrQueryColl.aggregate(pipeline).toArray()),
            );
        }
    }
}

function runTests() {
    sbeEligibleGroupDistinctScanTest();
    assertSbeWithCbrFallbackBehavior();
}

// TODO SERVER-117672: Improve handling of query knob setting and re-setting.
const prevCBRConfig = getCBRConfig(db);

try {
    // 1: Run with only MultiPlanning.
    db.adminCommand({setParameter: 1, featureFlagCostBasedRanker: false});
    runTests();

    // 2: Run with CBR fallback strategies.
    db.adminCommand({setParameter: 1, featureFlagCostBasedRanker: true, internalQueryCBRCEMode: "automaticCE"});

    const cbrFallbackStrategies = [
        "CBRForNoMultiplanningResults",
        "CBRCostBasedRankerChoice",
        "HistogramCEWithHeuristicFallback",
    ];

    for (const cbrFallbackStrategy of cbrFallbackStrategies) {
        jsTest.log.info("Running tests with automaticCE.", {cbrFallbackStrategy});
        assert.commandWorked(db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: cbrFallbackStrategy}));
        runTests();
    }

    // 3: Run under pure sampling configuration.
    db.adminCommand({setParameter: 1, internalQueryCBRCEMode: "samplingCE"});
    runTests();
} finally {
    setCBRConfig(db, prevCBRConfig);
}
