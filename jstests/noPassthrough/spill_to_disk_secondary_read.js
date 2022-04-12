/*
 * Test that $group and $setWindowFields spill to the WT RecordStore on secondaries with
 * writeConcern greater than w:1.
 * @tags: [requires_replication, requires_majority_read_concern]
 */
(function() {
"use strict";

load("jstests/libs/sbe_explain_helpers.js");  // For getSbePlanStages.
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.

const replTest = new ReplSetTest({
    nodes: 3,
});

replTest.startSet();
replTest.initiate();

// Test that spilling '$group' and '$lookup' pipeline on a secondary works with a writeConcern
// greater than w:1.
let primary = replTest.getPrimary();
const insertColl = primary.getDB("test").foo;
for (let i = 0; i < 500; ++i) {
    assert.commandWorked(insertColl.insert({a: i, string: "test test test"}));
}

let secondary = replTest.getSecondary();
assert.commandWorked(secondary.adminCommand(
    {setParameter: 1, internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: 1}));

assert.commandWorked(secondary.adminCommand({
    setParameter: 1,
    internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: 1
}));

const isSBELookupEnabled =
    checkSBEEnabled(secondary.getDB("test"), ["featureFlagSBELookupPushdown"]);

const readColl = secondary.getDB("test").foo;

let pipeline = [{$group: {_id: '$a', s: {$addToSet: '$string'}, p: {$push: '$a'}}}];

if (isSBELookupEnabled) {
    let explainRes = readColl.explain('executionStats').aggregate(pipeline, {allowDiskUse: true});
    const hashAggGroups = getSbePlanStages(explainRes, 'group');
    assert.eq(hashAggGroups.length, 1, explainRes);
    const hashAggGroup = hashAggGroups[0];
    assert(hashAggGroup, explainRes);
    assert(hashAggGroup.hasOwnProperty("usedDisk"), hashAggGroup);
    assert(hashAggGroup.usedDisk, hashAggGroup);
    assert.eq(hashAggGroup.spilledRecords, 500, hashAggGroup);
    assert.eq(hashAggGroup.spilledBytesApprox, 27500, hashAggGroup);
}

let res =
    readColl
        .aggregate(
            pipeline,
            {allowDiskUse: true, readConcern: {level: "majority"}, writeConcern: {"w": "majority"}})
        .toArray();

pipeline =
    [{$lookup: {from: readColl.getName(), localField: "a", foreignField: "a", as: "results"}}];

if (isSBELookupEnabled) {
    let explainRes = readColl.explain('executionStats').aggregate(pipeline, {allowDiskUse: true});
    const hLookups = getSbePlanStages(explainRes, 'hash_lookup');
    assert.eq(hLookups.length, 1, explainRes);
    const hLookup = hLookups[0];
    assert(hLookup, explainRes);
    assert(hLookup.hasOwnProperty("usedDisk"), hLookup);
    assert(hLookup.usedDisk, hLookup);
    assert.eq(hLookup.spilledRecords, 1000, hLookup);
    assert.eq(hLookup.spilledBytesApprox, 63000, hLookup);
}

res =
    readColl
        .aggregate(
            pipeline,
            {allowDiskUse: true, readConcern: {level: "majority"}, writeConcern: {"w": "majority"}})
        .toArray();

insertColl.drop();

// Test that spilling '$setWindowFields' pipeline on a secondary works with a writeConcern greater
// than w:1.
let avgDocSize = 274;
let smallPartitionSize = 6;
let largePartitionSize = 21;
const insertCollWFs = primary.getDB("test").bar;

// Create small partition.
for (let i = 0; i < smallPartitionSize; i++) {
    assert.commandWorked(insertCollWFs.insert({_id: i, val: i, partition: 1}));
}
// Create large partition.
for (let i = 0; i < largePartitionSize; i++) {
    assert.commandWorked(insertCollWFs.insert({_id: i + smallPartitionSize, val: i, partition: 2}));
}

assert.commandWorked(secondary.adminCommand({
    setParameter: 1,
    internalDocumentSourceSetWindowFieldsMaxMemoryBytes: largePartitionSize * avgDocSize + 1
}));

const readCollWFs = secondary.getDB("test").bar;

pipeline = [
    {
        $setWindowFields: {
            partitionBy: "$partition",
            sortBy: {partition: 1},
            output: {arr: {$push: "$val", window: {documents: [-25, 25]}}}
        }
    },
    {$sort: {_id: 1}}
];

res =
    readCollWFs
        .aggregate(
            pipeline,
            {allowDiskUse: true, readConcern: {level: "majority"}, writeConcern: {"w": "majority"}})
        .toArray();

replTest.stopSet();
})();
