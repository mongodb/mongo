/**
 * Creates a regular collection and a time series collection with block processing enabled, and runs
 * queries that result in different BSON types to ensure the results are the same.
 */

import {blockProcessingTestCases} from "jstests/libs/block_processing_test_cases.js";
import {leafs} from "jstests/query_golden/libs/example_data.js";

const scalarConn = MongoRunner.runMongod();
const bpConn = MongoRunner.runMongod(
    {setParameter: {featureFlagSbeFull: true, featureFlagTimeSeriesInSbe: true}});

assert.neq(null, scalarConn, "mongod was unable to start up");
assert.neq(null, bpConn, "mongod was unable to start up");

const scalarDb = scalarConn.getDB(jsTestName());
const bpDb = bpConn.getDB(jsTestName());

if (assert.commandWorked(scalarDb.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}))
        .internalQueryFrameworkControl == "forceClassicEngine") {
    jsTestLog("Skipping test due to forceClassicEngine");
    MongoRunner.stopMongod(scalarConn);
    MongoRunner.stopMongod(bpConn);
    quit();
}

const scalarColl = scalarDb.timeseries_group_bson_types;
const bpColl = bpDb.timeseries_group_bson_types;

const timeFieldName = 't';
const metaFieldName = 'm';

scalarColl.drop();
bpColl.drop();
// Create a TS collection to get block processing running. Compare this against a normal collection.
assert.commandWorked(bpDb.createCollection(bpColl.getName(), {
    timeseries: {timeField: timeFieldName, metaField: metaFieldName},
}));

const datePrefix = 1680912440;
const dateUpperBound = new Date(datePrefix + 500);
const dateLowerBound = new Date(datePrefix);

// TODO SERVER-85404 allow Min/MaxKey to be inserted once compression works.
let leafsMinusUndefined = leafs().filter(function(e) {
    return e !== MinKey && e !== MaxKey && typeof e !== "function" && e !== undefined;
});
// leafs has no long strings.
leafsMinusUndefined.push("a very long string");
// Add some nested array and object cases.
leafsMinusUndefined.push({a: 1});
leafsMinusUndefined.push({a: [1, 2, 3]});
leafsMinusUndefined.push({a: [[1, 2], [3, 4]]});
leafsMinusUndefined.push({a: [{b: 1}, {b: 2}]});
leafsMinusUndefined.push([{a: 1}, {a: 2}]);
leafsMinusUndefined.push([[{a: 1}, {a: 2}], [{a: 3}, {a: 4}]]);

// Push different types of data. Different meta fields, different permutations of leafs data in data
// fields.
const tsData = [];
for (const meta of leafsMinusUndefined) {
    let time = 0;
    for (const data of leafsMinusUndefined) {
        tsData.push({
            [timeFieldName]: new Date(datePrefix + time),
            [metaFieldName]: meta,
            x: data,
            y: data,
            z: data
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
            z: data1
        });
        tsData.push({
            [timeFieldName]: new Date(datePrefix + time),
            [metaFieldName]: 5,
            x: data1,
            y: data2,
            z: data1
        });
        tsData.push({
            [timeFieldName]: new Date(datePrefix + time),
            [metaFieldName]: 5,
            x: data1,
            y: data1,
            z: data2
        });
        time += 100;
    }
}

scalarColl.insert(tsData);
bpColl.insert(tsData);

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

    assert.eq(scalarResults.length, bpResults.length);
    // Sort the data so we can compare by iterating through both results pairwise.
    const cmpFn = function(doc1, doc2) {
        return tojson(doc1) < tojson(doc2);
    };
    scalarResults.sort(cmpFn);
    bpResults.sort(cmpFn);
    for (let i = 0; i < scalarResults.length; i++) {
        const scalarKeys = Object.keys(scalarResults[i]);
        const bpKeys = Object.keys(bpResults[i]);
        assert.eq(scalarKeys, bpKeys);

        for (const key of scalarKeys) {
            const scalarField = scalarResults[i][key];
            const bpField = bpResults[i][key];
            // NaN is not equal to NaN.
            if (isNaN(scalarField) && isNaN(bpField)) {
                continue;
            }
            assert.eq(scalarField, bpField, {name: test.name, index: i})
        }
    }
}

function runTestCases(allowDiskUse, forceSpilling) {
    assert.commandWorked(bpDb.adminCommand({
        setParameter: 1,
        internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling: forceSpilling
    }));
    let testcases = blockProcessingTestCases(
        timeFieldName, metaFieldName, datePrefix, dateUpperBound, dateLowerBound, false);
    // Filter out tests with known accepted differences between SBE and Classic.
    testcases = testcases.filter(function(test) {
        return test.name !== 'MinOfMetaSortKey_GroupByX';
    });
    for (const test of testcases) {
        compareScalarAndBlockProcessing(test, allowDiskUse);
    }
}

// Run with different combinations of allowDiskUse and forced spilling.
runTestCases(false /*allowDiskUse*/, false /*forceSpilling*/);
runTestCases(true /*allowDiskUse*/, false /*forceSpilling*/);
runTestCases(true /*allowDiskUse*/, true /*forceSpilling*/);

MongoRunner.stopMongod(scalarConn);
MongoRunner.stopMongod(bpConn);
