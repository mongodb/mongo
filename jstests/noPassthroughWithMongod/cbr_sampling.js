/**
 * Test that cost-based ranking can use sampling to estimate filters.
 */

import {
    getRejectedPlans,
    getWinningPlanFromExplain,
    isCollscan,
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

let docs = [];
for (let i = 0; i < 1000; i++) {
    let doc = {a: i, b: [i - 1, i, i + 1]};
    if (i % 2 === 0) {
        doc.c = i;
    }
    docs.push(doc);
}

assert.commandWorked(coll.insert(docs));

function assertCollscanUsesSampling(query) {
    const explain = coll.find(query).explain();
    const plan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, plan));
    assert.eq(plan.estimatesMetadata.ceSource, "Sampling", plan);
}

function assertAllPlansUseSampling(query) {
    const explain = coll.find(query).explain();
    [getWinningPlanFromExplain(explain), ...getRejectedPlans(explain)].forEach(plan => {
        assert.eq(plan.estimatesMetadata.ceSource, "Sampling", plan);
        assert.gt(plan.cardinalityEstimate, 0, plan);
        assert.gt(plan.inputStage.cardinalityEstimate, 0, plan);
        assert.gt(plan.inputStage.numKeysEstimate, 0, plan);
    });
}

try {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "samplingCE"}));
    assertCollscanUsesSampling({a: {$lt: 100}});
    assertCollscanUsesSampling({b: {$elemMatch: {$gt: 100, $lt: 200}}});
    // Test the chunk-based sampling method.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQuerySamplingCEMethod: "chunk"}));
    assertCollscanUsesSampling({a: {$lt: 1000}});

    // Switch back to the random sampling method because the CE could be 0 for some of the filters
    // below, for example, the ce of 'b: {$lt: 500}' is 0 if all the chunks picked happen to fall
    // into the first half of the collection.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQuerySamplingCEMethod: "random"}));

    // TODO SERVER-97790: Enable rooted $or test
    // assertCollscanUsesSampling({$or: [{a: {$lt: 100}}, {b: {$gt: 900}}]});

    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assertAllPlansUseSampling({a: {$lt: 100}});
    assertAllPlansUseSampling({b: {$lt: 100}});
    assertAllPlansUseSampling({a: {$lt: 100}, b: {$lt: 500}});
    assertAllPlansUseSampling({a: {$lt: 100}, b: {$lt: 500}, c: {$exists: true}});
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
