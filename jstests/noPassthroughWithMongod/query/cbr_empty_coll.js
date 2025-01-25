/**
 * Test that CBR is able to properly estimate empty collections.
 */
import {
    getRejectedPlans,
    getWinningPlanFromExplain,
    isCollscan,
    isEofPlan,
    isIxscan,
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();

function testNonExistentColl() {
    // Non-existent namespace results in EOF plan that does not go through CBR
    coll.drop();
    const nonExistentCollPlan = getWinningPlanFromExplain(coll.find({a: 1}).explain());
    assert(isEofPlan(db, nonExistentCollPlan), nonExistentCollPlan);
    assert(!nonExistentCollPlan.hasOwnProperty("cardinalityEstimate"));
}

function assertAllPlansHaveZeroCE(explainRoot) {
    [getWinningPlanFromExplain(explainRoot), ...getRejectedPlans(explainRoot)].forEach(plan => {
        assert.eq(plan.cardinalityEstimate, 0, plan);
    });
}

function testEmptyColl() {
    // Ensure collection exists and is empty
    const collNames = db.getCollectionNames();
    if (collNames.includes(collName)) {
        assert.eq(coll.count(), 0);
    } else {
        assert.commandWorked(db.createCollection(collName));
    }

    // Empty collection results in a Collscan that goes through CBR
    const emptyCollPlan = getWinningPlanFromExplain(coll.find({a: 1}).explain());
    assert(isCollscan(db, emptyCollPlan), emptyCollPlan);
    assert.eq(emptyCollPlan.cardinalityEstimate, 0, emptyCollPlan);

    // Create an index and run the same query
    assert.commandWorked(coll.createIndex({a: 1}));
    const emptyCollIndexPlan = getWinningPlanFromExplain(coll.find({a: 1}).explain());
    assert(isIxscan(db, emptyCollIndexPlan), emptyCollIndexPlan);
    assert.eq(emptyCollIndexPlan.cardinalityEstimate, 0, emptyCollIndexPlan);

    // Index intersection
    assert.commandWorked(coll.createIndex({b: 1}));
    const emptyCollIntersectionExp = coll.find({a: 1, b: 1}).explain();
    assertAllPlansHaveZeroCE(emptyCollIntersectionExp);

    // Index union
    const emptyCollUnionExp = coll.find({
                                      $and: [
                                          {$or: [{a: 10}, {b: {$gt: 99}}]},
                                          {$or: [{a: {$in: [5, 1]}}, {b: {$in: [7, 99]}}]}
                                      ]
                                  })
                                  .explain();
    assertAllPlansHaveZeroCE(emptyCollUnionExp);

    // Test limit and skip with empty collection
    const emptyCollLimitPlan = getWinningPlanFromExplain(coll.find({a: 1}).limit(5).explain());
    assert.eq(emptyCollLimitPlan.cardinalityEstimate, 0, emptyCollLimitPlan);
    const emptyCollSkipPlan = getWinningPlanFromExplain(coll.find({a: 1}).skip(5).explain());
    assert.eq(emptyCollSkipPlan.cardinalityEstimate, 0, emptyCollSkipPlan);

    // Cleanup indexes created in this testcase
    assert.commandWorked(coll.dropIndex({a: 1}));
    assert.commandWorked(coll.dropIndex({b: 1}));
}

function createHistogram(field) {
    // Creating a histogram requires at least one document
    assert.commandWorked(coll.insert({_id: 1, [field]: 1}));
    assert.commandWorked(coll.runCommand({analyze: collName, key: field}));
    // Delete the document to get an empty collection
    assert.commandWorked(coll.deleteOne({_id: 1}));
}

try {
    // TODO SERVER-99347: Add test with samplingCE
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "heuristicCE"}));
    testNonExistentColl();
    testEmptyColl();

    {
        createHistogram("a");
        createHistogram("b");
        assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));
        testEmptyColl();
    }
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
