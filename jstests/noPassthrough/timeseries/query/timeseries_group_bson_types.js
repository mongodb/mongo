/**
 * Creates a regular collection and a time series collection with block processing enabled, and runs
 * queries that result in different BSON types to ensure the results are the same.
 */

import {
    blockProcessingTestCases,
    generateMetaVals
} from "jstests/libs/query/block_processing_test_cases.js";
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

const scalarColl = scalarDb[jsTestName()];
const bpColl = bpDb[jsTestName()];

const timeFieldName = 'time';
const metaFieldName = 'meta';

scalarColl.drop();
bpColl.drop();
// Create a TS collection to get block processing running. Compare this against a normal collection.
assert.commandWorked(bpDb.createCollection(bpColl.getName(), {
    timeseries: {timeField: timeFieldName, metaField: metaFieldName},
}));

const datePrefix = 1680912440;
const dateUpperBound = new Date(datePrefix + 500);
const dateLowerBound = new Date(datePrefix);

let leafsMinusUndefined = leafs().filter(function(e) {
    return typeof e !== "function" && e !== undefined;
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
for (const meta of leafsMinusUndefined.concat(generateMetaVals())) {
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

assert.commandWorked(scalarColl.insert(tsData));
assert.commandWorked(bpColl.insert(tsData));

function isNumberDecimal(num) {
    return (num + "").startsWith('NumberDecimal');
}

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
        const doc1Json = tojson(doc1);
        const doc2Json = tojson(doc2);
        return doc1Json < doc2Json ? -1 : (doc1Json > doc2Json ? 1 : 0);
    };

    const normalizeNaN = function(arg) {
        if (Number.isNaN(arg)) {
            return NumberDecimal("NaN");
        } else if (arg !== null && (arg.constructor === Object || Array.isArray(arg))) {
            let newArg = Array.isArray(arg) ? [] : {};
            for (let prop in arg) {
                newArg[prop] = normalizeNaN(arg[prop]);
            }
            return newArg;
        }
        return arg;
    };

    scalarResults = normalizeNaN(scalarResults);
    bpResults = normalizeNaN(bpResults);

    scalarResults.sort(cmpFn);
    bpResults.sort(cmpFn);
    for (let i = 0; i < scalarResults.length; i++) {
        const scalarKeys = Object.keys(scalarResults[i]);
        const bpKeys = Object.keys(bpResults[i]);
        assert.eq(scalarKeys, bpKeys);

        for (const key of scalarKeys) {
            const debugInfo = {name: test.name, resultIndex: i};
            const scalarField = scalarResults[i][key];
            const bpField = bpResults[i][key];
            // If the values are NumberDecimal, we have to call .equals().
            const scalarIsDec = isNumberDecimal(scalarField);
            const bpIsDec = isNumberDecimal(bpField);
            if (scalarIsDec && !bpIsDec) {
                assert(scalarField.equals(NumberDecimal(bpField)), debugInfo);
            } else if (!scalarIsDec && bpIsDec) {
                assert(bpField.equals(NumberDecimal(scalarField)), debugInfo);
            } else {
                assert.eq(scalarField, bpField, debugInfo);
            }
        }
    }
}

function runTestCases(allowDiskUse, forceSpilling) {
    assert.commandWorked(bpDb.adminCommand({
        setParameter: 1,
        internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling: forceSpilling
    }));
    let testcases = blockProcessingTestCases(
        timeFieldName, metaFieldName, datePrefix, dateUpperBound, dateLowerBound, false, false);
    // Filter out tests with known accepted differences between SBE and Classic.
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
