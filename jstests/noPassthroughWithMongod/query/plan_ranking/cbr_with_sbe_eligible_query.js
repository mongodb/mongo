import {resultsEq} from "jstests/aggregation/extras/utils.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();
coll.insert({_id: 0, a: 0});

function sbeEligibleGroupDistinctScanTest() {
    jsTest.log.info("Test Case: sbeEligibleGroupDistinctScanTest");
    const pipeline = [{$match: {a: 0}}, {$group: {_id: "$_id"}}];
    const results = coll.aggregate(pipeline).toArray();
    assert(resultsEq(results, [{_id: 0}]));
}

// TODO SERVER-117672: Improve handling of query knob setting and re-setting.
const prevQueryKnobs = assert.commandWorked(
    db.adminCommand({
        getParameter: 1,
        planRankerMode: 1,
        automaticCEPlanRankingStrategy: 1,
    }),
);
const prevPlanRankerMode = prevQueryKnobs.planRankerMode;
const prevAutomaticCEPlanRankingStrategy = prevQueryKnobs.automaticCEPlanRankingStrategy;

try {
    // 1: Run with only MultiPlanning.
    db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"});
    sbeEligibleGroupDistinctScanTest();

    // 2: Run with CBR fallback strategies.
    db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"});

    const cbrFallbackStrategies = [
        "CBRForNoMultiplanningResults",
        "CBRCostBasedRankerChoice",
        "HistogramCEWithHeuristicFallback",
    ];

    for (const cbrFallbackStrategy of cbrFallbackStrategies) {
        jsTest.log.info("Running tests with automaticCE.", {cbrFallbackStrategy});
        sbeEligibleGroupDistinctScanTest();
    }

    // 3: Run under pure sampling configuration.
    db.adminCommand({setParameter: 1, planRankerMode: "samplingCE"});
    sbeEligibleGroupDistinctScanTest();
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: prevPlanRankerMode}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, automaticCEPlanRankingStrategy: prevAutomaticCEPlanRankingStrategy}),
    );
}
