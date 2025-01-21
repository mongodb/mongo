/**
 * Ensure that the analyze command produces histograms which cost-based ranking is able to use.
 */

import {
    canonicalizePlan,
    getRejectedPlans,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {ceEqual} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
const histogramColl = db.system.statistics[collName];
coll.drop();
histogramColl.drop();

// Generate docs with decreasing distribution
let docs = [];
for (let i = 0; i < 100; i++) {
    for (let j = 0; j < i; j++) {
        docs.push({a: j, b: i % 5, c: i % 7});
    }
}
assert.commandWorked(coll.insertMany(docs));

coll.createIndex({a: 1});

// Generate histograms for field 'a' and 'b'
assert.commandWorked(coll.runCommand({analyze: collName, key: "a", numberBuckets: 10}));
assert.commandWorked(coll.runCommand({analyze: collName, key: "b"}));
assert.commandWorked(coll.runCommand({analyze: collName, key: "c"}));

// Run a find command with the given filter and verify that every plan uses a histogram estimate.
function assertQueryUsesHistograms({query, expectedCE}) {
    const explain = coll.find(query).explain();
    jsTestLog(explain);
    [getWinningPlanFromExplain(explain), ...getRejectedPlans(explain)].forEach(plan => {
        assert.eq(plan.estimatesMetadata.ceSource, "Histogram", plan);
        assert(plan.hasOwnProperty("cardinalityEstimate"));
        const gotCE = plan.cardinalityEstimate;
        assert.gt(gotCE, 0);
        assert(ceEqual(gotCE, expectedCE),
               `Got CE: ${gotCE} and expected CE: ${expectedCE} for query: ${tojson(query)}`);
    });
}

try {
    // Use histogram CE
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));
    const testCases = [
        // IndexScan should use histogram
        {query: {a: 5}, expectedCE: 94},
        {query: {a: {$gt: 5}}, expectedCE: 4371},
        {query: {a: {$lt: 5}}, expectedCE: 485},
        // CollScan with sargable filter should use histogram
        {query: {b: 4}, expectedCE: 1030},
        {query: {b: {$gt: 3}}, expectedCE: 1030},
        {query: {b: {$lt: 3}}, expectedCE: 2910},
        // Conjunctions
        {query: {b: {$gte: 1, $lt: 3}}, expectedCE: 1960},
        {query: {a: 5, b: {$gte: 1, $lt: 3}}, expectedCE: 59.1},
        {query: {b: {$gte: 1, $lte: 3}, c: {$gt: 0, $lt: 5}}, expectedCE: 2158.8},
        {
            query: {$and: [{b: {$gte: 1}}, {c: {$gt: 0}}, {b: {$lte: 3}}, {c: {$lt: 5}}]},
            expectedCE: 2158.8,
        },
        // Negations
        {query: {a: {$lt: 5, $ne: 6}}, expectedCE: 485.0}
    ];
    testCases.forEach(tc => assertQueryUsesHistograms(tc));
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}

try {
    // Test if both nulls and missings are counted in the histogram estimate.
    coll.drop();
    histogramColl.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.insertMany([
        {a: null, b: 0},
        {a: null, b: 1},
        {a: null, b: 2},
        {a: null, b: 3},
        {b: 4},
        {b: 5},
        {b: 6}
    ]));
    assert.commandWorked(coll.runCommand({analyze: collName, key: "a", numberBuckets: 10}));
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));
    const explain = coll.find({a: null}).explain();
    [getWinningPlanFromExplain(explain), ...getRejectedPlans(explain)].forEach(plan => {
        assert.eq(plan.estimatesMetadata.ceSource, "Histogram", plan);
        assert.close(plan.cardinalityEstimate, 7);
    });
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}

try {
    // Test CE module makse us of multikey metadata
    coll.drop();
    histogramColl.drop();
    let docs = [];
    for (let i = 0; i < 100; i++) {
        docs.push({a: i});
    }
    assert.commandWorked(coll.insertMany(docs));
    // Create index so the catalog has multikey metadata, but the queries we run hint a collection
    // scan so we can test CE of MatchExpressions using histogram.
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.runCommand({analyze: collName, key: "a", numberBuckets: 10}));
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));

    const query = {a: {$gt: 10, $lt: 20}};
    const nonMultikeyEstimate =
        getWinningPlanFromExplain(coll.find(query).hint({$natural: 1}).explain())
            .cardinalityEstimate;

    // Make index on 'a' multikey
    assert.commandWorked(coll.insert({_id: 1, a: [100, 101, 102]}));
    assert.commandWorked(coll.deleteOne({_id: 1}));

    const multiKeyEstimate =
        getWinningPlanFromExplain(coll.find(query).hint({$natural: 1}).explain())
            .cardinalityEstimate;

    // CBR can generate (10,20) interval in the non-multikey case, but must estimate [-inf, 20) &
    // (10, inf] intervals in the multikey case.
    assert.lt(nonMultikeyEstimate, multiKeyEstimate);
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}

try {
    // Test histogram CE can estimate compound index scan with only a prefix of fields having
    // histograms.
    coll.drop();
    histogramColl.drop();

    let docs = [];
    for (let i = 0; i < 100; i++) {
        docs.push({a: i, b: 100 - i});
    }
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndexes([
        {a: 1, c: 1},
        {a: 1, b: 1, c: 1},
    ]));
    assert.commandWorked(coll.runCommand({analyze: collName, key: "a", numberBuckets: 10}));
    assert.commandWorked(coll.runCommand({analyze: collName, key: "b", numberBuckets: 10}));
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));

    const testCases = [
        {query: {a: 20}, expectedCE: 1},
        {query: {a: {$gt: 20}}, expectedCE: 79.1},
        {query: {a: {$gt: 20, $lt: 40}}, expectedCE: 18.8},
        {query: {a: 20, b: 20}, expectedCE: 0.1},
        {query: {a: 20, b: {$gt: 20}}, expectedCE: 0.9},
        {query: {a: {$gt: 20}, b: 20}, expectedCE: 0.9},
        {query: {a: {$gt: 20}, b: {$gt: 20}}, expectedCE: 70.8},
    ];
    testCases.forEach(tc => assertQueryUsesHistograms(tc));
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
