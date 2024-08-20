/**
 * This test exercises the block processing $topN and $bottomN accumulators for $group with a focus
 * on edge cases. It is aimed at testing the accumulators, not the $group stage itself. It compares
 * the results between SBE using $group block processing and Classic engine.
 */

import {leafs} from "jstests/query_golden/libs/example_data.js";

const classicConn =
    MongoRunner.runMongod({setParameter: {internalQueryFrameworkControl: "forceClassicEngine"}});
const bpConn = MongoRunner.runMongod(
    {setParameter: {featureFlagSbeFull: true, featureFlagTimeSeriesInSbe: true}});

assert.neq(null, classicConn, "classicConn mongod was unable to start up");
assert.neq(null, bpConn, "bpConn mongod was unable to start up");

const classicDb = classicConn.getDB(jsTestName());
const bpDb = bpConn.getDB(jsTestName());

const classicColl = classicDb.timeseries_group_aggregations;
const bpColl = bpDb.timeseries_group_aggregations;

classicColl.drop();
bpColl.drop();
// Create a TS collection to get block processing running. Compare this against a classic
// collection.
assert.commandWorked(bpDb.createCollection(bpColl.getName(), {
    timeseries: {timeField: 'time', metaField: 'meta'},
}));

// 'bsonVals' is an array containing all types of BSON values, including extreme values per type.
let bsonVals = leafs();
// leafs() only returns small and medium length strings
bsonVals.push("A much longer string..............................................................");
const numBsonVals = bsonVals.length;
// We want 'numBsonVals' to be a prime number to avoid harmonics in the test case generation.
assert.eq(numBsonVals, 73, "Expected numBsonVals to be the prime number 73.");

const datePrefix = 1680912440;

// Create time series buckets with different time and meta fields, and data fields 'w','x','y','z'.
const tsDocs = [];
let id = 0;
for (let metaIdx = 0; metaIdx < 10; ++metaIdx) {
    const metaDoc = {metaA: metaIdx, metaB: metaIdx / 2};

    let currentDate = 0;
    // Define a prime number of docs for this metadata. The data offsets also use prime factors.
    for (let doc = 0; doc < 37; ++doc) {
        tsDocs.push({
            _id: id++,
            time: new Date(datePrefix + currentDate),
            meta: metaDoc,
            w: bsonVals[id % numBsonVals],
            x: bsonVals[(1 + 5 * id) % numBsonVals],
            y: bsonVals[(3 + 11 * id) % numBsonVals],
            z: bsonVals[(7 + 19 * id) % numBsonVals]
        });
        currentDate += 25;
    }
}
const numTsDocs = tsDocs.length;

assert.commandWorked(classicColl.insert(tsDocs));
assert.commandWorked(bpColl.insert(tsDocs));

function compareClassicAndBP(pipeline, allowDiskUse) {
    const classicResults = classicColl.aggregate(pipeline, {allowDiskUse}).toArray();
    const bpResults = bpColl.aggregate(pipeline, {allowDiskUse}).toArray();

    // Sort order is not guaranteed, so let's sort by the object itself before comparing.
    const cmpFn = function(doc1, doc2) {
        const doc1Json = tojson(doc1);
        const doc2Json = tojson(doc2);
        return doc1Json < doc2Json ? -1 : (doc1Json > doc2Json ? 1 : 0);
    };
    classicResults.sort(cmpFn);
    bpResults.sort(cmpFn);

    function errFn() {
        jsTestLog(classicColl.explain().aggregate(pipeline, {allowDiskUse}));
        jsTestLog(bpColl.explain().aggregate(pipeline, {allowDiskUse}));

        return "compareClassicAndBP: Got different results for pipeline " + tojson(pipeline);
    }
    assert.eq(classicResults, bpResults, errFn);
}

// Group IDs of varying complexities.
const complexGroups = [
    {w: "$w"},
    {w: "$w", x: "$x"},
    {w: "$w", x: "$x", y: "$y"},
    {w: "$w", x: "$x", y: "$y", z: "$z"},
    {time: "$time"},
    {time: "$time", w: "$w"},
    {meta: "$meta"},
    {meta: "$meta", x: "$x"},
    {time: "$time", meta: "$meta", y: "$y"},
    {time: "$time", meta: "$meta", y: "$y", z: "$z"},
];

// Values to use for the "n" argument of $topN and $bottomN. This can only be a positive
// integral type, else it gets rejected during query parsing. We do not need to test invalid
// values here as parsing is not part of this feature.
const complexNVals = [
    numTsDocs - 1,
    numTsDocs,
    numTsDocs + 1,
    NumberInt(1),
    NumberInt(10),
    NumberInt('+2147483647'),
    NumberLong(3),
    NumberLong(50),
    NumberLong('+9223372036854775807'),
];

// sortBy values of varying complexities, including duplicate fields (which eslint doesn't like).
/* eslint-disable */
const complexSortBys = [
    {w: 1, x: 1, y: -1, z: -1, _id: 1},
    {w: 1, w: 1, _id: 1},
    {w: 1, w: -1, _id: 1},
    {w: -1, x: 1, w: -1, x: -1, y: 1, z: 1, x: 1, _id: 1},
];
/* eslint-enable */

// Define the $group stages.
let sortDir = 1;
let groupStages = [];
for (const accumulator of ['$topN', '$bottomN']) {
    // Complex group ID.
    for (const group of complexGroups) {
        groupStages.push({
            $group: {_id: group, acc: {[accumulator]: {n: 30, sortBy: {_id: 1}, output: ["$_id"]}}}
        });
    }

    // Complex n.
    for (const nVal of complexNVals) {
        for (const sortBy of ['w', 'x', 'y', 'z']) {
            groupStages.push({
                $group: {
                    _id: null,
                    acc: {
                        [accumulator]:
                            {n: nVal, sortBy: {[sortBy]: sortDir, _id: 1}, output: ["$_id"]}
                    }
                }
            });
            sortDir *= -1;
        }
    }

    // Complex sortBy.
    for (const sortBy of complexSortBys) {
        groupStages.push(
            {$group: {_id: null, acc: {[accumulator]: {n: 40, sortBy: sortBy, output: ["$_id"]}}}});
    }

    // Complex group and sortBy.
    for (const group of complexGroups) {
        for (const sortBy of complexSortBys) {
            groupStages.push({
                $group:
                    {_id: group, acc: {[accumulator]: {n: 50, sortBy: sortBy, output: ["$_id"]}}}
            });
        }
    }
}  // for each accumulator

// Add some queries with multiple $topN and/or $bottomN accumulators.
groupStages.push({
    $group: {
        _id: {time: "$time", w: "$w"},
        acc1: {$topN: {n: 20, sortBy: {x: 1, _id: 1}, output: ["$_id"]}},
        acc2: {$bottomN: {n: 25, sortBy: {y: 1, z: -1, _id: 1}, output: ["$_id"]}},
    }
});
groupStages.push({
    $group: {
        _id: {meta: "$meta", time: "$time"},
        acc1: {$topN: {n: 11, sortBy: {w: 1, x: -1, _id: 1}, output: ["$_id"]}},
        acc2: {$bottomN: {n: 13, sortBy: {w: 1, x: -1, _id: 1}, output: ["$_id"]}},
        acc3: {$topN: {n: 17, sortBy: {w: 1, x: 1, y: 1, _id: 1}, output: ["$_id"]}},
        acc4: {$bottomN: {n: 19, sortBy: {x: -1, y: -1, z: -1, _id: 1}, output: ["$_id"]}},
    }
});

function runAggregations(allowDiskUse, forceSpilling) {
    // Don't set the flags on classic because it's already considered correct.
    assert.commandWorked(bpDb.adminCommand({
        setParameter: 1,
        internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling: forceSpilling
    }));
    let pipe = 0;
    for (const groupStage of groupStages) {
        compareClassicAndBP([groupStage], allowDiskUse);
        ++pipe;
    }
    jsTestLog(`runAggregations: Ran ${pipe} pipelines with allowDisk=${allowDiskUse},
                forceSpilling=${forceSpilling}`);
}  // runAggregations

// Run with different combinations of allowDiskUse and force spilling.
runAggregations(false /* allowDiskUse */, false /* forceSpilling */);

MongoRunner.stopMongod(classicConn);
MongoRunner.stopMongod(bpConn);
