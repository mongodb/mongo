/**
 * Creates a regular collection and a time series collection with block processing enabled, and runs
 * queries that result in different BSON types to ensure the results are the same.
 */

import {blockProcessingTestCases, generateMetaVals} from "jstests/libs/query/block_processing_test_cases.js";
import {leafs} from "jstests/query_golden/libs/example_data.js";

const scalarConn = MongoRunner.runMongod();
const bpConn = MongoRunner.runMongod({setParameter: {featureFlagSbeFull: true, featureFlagTimeSeriesInSbe: true}});

assert.neq(null, scalarConn, "mongod was unable to start up");
assert.neq(null, bpConn, "mongod was unable to start up");

const scalarDb = scalarConn.getDB(jsTestName());
const bpDb = bpConn.getDB(jsTestName());

if (
    assert.commandWorked(scalarDb.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl == "forceClassicEngine"
) {
    jsTestLog("Skipping test due to forceClassicEngine");
    MongoRunner.stopMongod(scalarConn);
    MongoRunner.stopMongod(bpConn);
    quit();
}

const scalarColl = scalarDb[jsTestName()];
const bpColl = bpDb[jsTestName()];

const timeFieldName = "time";
const metaFieldName = "m";

scalarColl.drop();
bpColl.drop();
// Create a TS collection to get block processing running. Compare this against a normal collection.
assert.commandWorked(
    bpDb.createCollection(bpColl.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    }),
);

const datePrefix = 1680912440;
const dateUpperBound = new Date(datePrefix + 500);
const dateLowerBound = new Date(datePrefix);

let leafsMinusUndefined = leafs().filter(function (e) {
    return typeof e !== "function" && e !== undefined;
});
// leafs has no long strings.
leafsMinusUndefined.push("a very long string");
// Add some nested array and object cases.
leafsMinusUndefined.push({a: 1});
leafsMinusUndefined.push({a: [1, 2, 3]});
leafsMinusUndefined.push({
    a: [
        [1, 2],
        [3, 4],
    ],
});
leafsMinusUndefined.push({a: [{b: 1}, {b: 2}]});
leafsMinusUndefined.push([{a: 1}, {a: 2}]);
leafsMinusUndefined.push([
    [{a: 1}, {a: 2}],
    [{a: 3}, {a: 4}],
]);

// Push different types of data. Different meta fields, different permutations of leafs data in data
// fields.
const tsData = [];
for (const meta of leafsMinusUndefined.concat(generateMetaVals())) {
    let time = 0;
    for (const data of leafsMinusUndefined) {
        tsData.push({
            [timeFieldName]: new Date(datePrefix + time),
            [metaFieldName]: meta,
            x: data,
            y: data,
            z: data,
        });
        time += 100;
    }
}
for (const data1 of leafsMinusUndefined) {
    let time = 0;
    for (const data2 of leafsMinusUndefined) {
        tsData.push({
            [timeFieldName]: new Date(datePrefix + time),
            [metaFieldName]: 5,
            x: data2,
            y: data1,
            z: data1,
        });
        tsData.push({
            [timeFieldName]: new Date(datePrefix + time),
            [metaFieldName]: 5,
            x: data1,
            y: data2,
            z: data1,
        });
        tsData.push({
            [timeFieldName]: new Date(datePrefix + time),
            [metaFieldName]: 5,
            x: data1,
            y: data1,
            z: data2,
        });
        time += 100;
    }
}

assert.commandWorked(scalarColl.insert(tsData));
assert.commandWorked(bpColl.insert(tsData));

function compareScalarAndBlockProcessing(test, allowDiskUse) {
    let scalarFailed = false;
    let scalarResults = [];
    try {
        scalarResults = scalarColl.aggregate(test.pipeline, {allowDiskUse}).toArray();
    } catch {
        scalarFailed = true;
    }

    let bpFailed = false;
    let bpResults = [];
    try {
        bpResults = bpColl.aggregate(test.pipeline, {allowDiskUse}).toArray();
    } catch {
        bpFailed = true;
    }

    assert.eq(scalarFailed, bpFailed);
    if (scalarFailed) {
        return;
    }

    // `_resultSetsEqualNormalized` normalizes number types (included NaN), sorts between documents
    // and within documents, and then returns true or false if the result sets are equal.
    assert(_resultSetsEqualNormalized(scalarResults, bpResults), {test, scalarResults, bpResults});
}

function runTestCases(allowDiskUse, forceSpilling) {
    assert.commandWorked(
        bpDb.adminCommand({setParameter: 1, internalQuerySlotBasedExecutionHashAggIncreasedSpilling: forceSpilling}),
    );
    let testcases = blockProcessingTestCases(
        timeFieldName,
        metaFieldName,
        datePrefix,
        dateUpperBound,
        dateLowerBound,
        false,
        false,
    );
    // Filter out tests with known accepted differences between SBE and Classic.
    for (const test of testcases) {
        compareScalarAndBlockProcessing(test, allowDiskUse);
    }
}

// Run with different combinations of allowDiskUse and increased spilling in debug builds.
runTestCases(false /*allowDiskUse*/, "inDebug" /*increased Spilling*/);
runTestCases(true /*allowDiskUse*/, "inDebug" /*increased Spilling*/);
runTestCases(true /*allowDiskUse*/, "always" /*increased Spilling*/);

MongoRunner.stopMongod(scalarConn);
MongoRunner.stopMongod(bpConn);
