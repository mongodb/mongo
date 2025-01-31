/**
 * Verify CBR can estimate value $elemMatch MatchExpression using histograms.
 */
import {
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {planEstimatedWithHistogram} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();

let docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({
        a: [
            i,
            i * 2,
            i * 3,
        ]
    });
}
assert.commandWorked(coll.insert(docs));

function testElemMatchWithDifferentSelectivities() {
    coll.dropIndexes();

    assert.commandWorked(coll.runCommand({analyze: collName, key: "a"}));
    // No index on 'a' forces a collection scan plan.

    const moreDocsPlan =
        getWinningPlanFromExplain(coll.find({a: {$elemMatch: {$gt: 5, $lt: 20}}}).explain());
    const fewerDocsPlan =
        getWinningPlanFromExplain(coll.find({a: {$elemMatch: {$gt: 5, $lt: 15}}}).explain());

    assert(planEstimatedWithHistogram(moreDocsPlan), moreDocsPlan);
    assert(planEstimatedWithHistogram(fewerDocsPlan), fewerDocsPlan);
    assert.lt(fewerDocsPlan.cardinalityEstimate, moreDocsPlan.cardinalityEstimate);
}

function testIndexDoesNotAffectEstimate() {
    coll.dropIndexes();
    assert.commandWorked(coll.runCommand({analyze: collName, key: "a"}));

    const query = {a: {$elemMatch: {$gt: 5, $lt: 20}}};
    const noIndexPlan = getWinningPlanFromExplain(coll.find(query).explain());
    assert.commandWorked(coll.createIndex({a: 1}));
    const indexPlan = getWinningPlanFromExplain(coll.find(query).explain());

    assert(planEstimatedWithHistogram(noIndexPlan), noIndexPlan);
    assert(planEstimatedWithHistogram(indexPlan), indexPlan);
    // The no index plan will have the estimate on the root collscan stage. The index plan will have
    // the equivalent estimate on the index scan as our implementation is not robust to "double
    // counting".
    // TODO SERVER-98094: Once we can detect this case, change this comparison to the root stages.
    assert.eq(noIndexPlan.cardinalityEstimate, indexPlan.inputStage.cardinalityEstimate);
}

try {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));
    testElemMatchWithDifferentSelectivities();
    testIndexDoesNotAffectEstimate();
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
