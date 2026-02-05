import {resultsEq} from "jstests/aggregation/extras/utils.js";
import {checkSbeStatus, kSbeDisabled} from "jstests/libs/query/sbe_util.js";
import {getCBRConfig, restoreCBRConfig} from "jstests/libs/query/cbr_utils.js";

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

function sbeEligibleQueriesDoNotUseFallbackCode() {
    // Regardless of the featureFlagCostBasedRanker, a query that is going to be executed
    // with SBE should not go down the CBR fallback codepath. Instead it should be planned
    // as if the feature flag was disabled. We check this by looking at the rejected
    // plans - if the query was planned via the CBR fallback codepath then the rejected
    // plans in the explain output would not be populated because the SBE plan explainer
    // has not yet been fixed to work with plans going down the fallback route (SERVER-92589).
    // TODO SERVER-117707: Modify/delete this test.
    jsTest.log.info("Test Case: sbeEligibleQueriesDoNotUseFallbackCode");

    // The assertions below only make sense when the queries are eligible for SBE, i.e.
    // when the query framework control is trySbeEngine or higher.
    if (checkSbeStatus(db) == kSbeDisabled) {
        return;
    }

    const pipelines = [
        // Queries that would not fallback to CBR if we allowed SBE plans to be planned via the fallback.
        [{$match: {a: 20000, b: 20000, c: 1}}, {$group: {_id: "$a"}}], // distinct
        [{$match: {a: 20000, b: 20000, c: 1}}, {$group: {_id: null}}], // non-distinct
        // Queries that would fallback to CBR if we allowed SBE plans to be planned via the fallback.
        [{$match: {a: {$gte: 1}, b: {$gte: 1}, c: 1}}, {$group: {_id: "$a"}}], // distinct
        [{$match: {a: {$gte: 1}, b: {$gte: 1}, c: 1}}, {$group: {_id: null}}], // non-distinct
    ];

    for (const pipeline of pipelines) {
        jsTest.log.info("Pipeline: " + tojson(pipeline));

        const explain = cbrQueryColl.explain().aggregate(pipeline);
        // Observing the rejected plan means we used pure multiplanning for the query.
        assert.eq(explain.queryPlanner.rejectedPlans.length, 1, tojson(explain));

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
    sbeEligibleQueriesDoNotUseFallbackCode();
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
    restoreCBRConfig(db, prevCBRConfig);
}
