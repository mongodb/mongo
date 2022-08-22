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

function setLog(db) {
    db.setLogLevel(5, 'command');
    db.setLogLevel(5, 'index');
    db.setLogLevel(5, 'query');
    db.setLogLevel(5, 'replication');
    db.setLogLevel(5, 'write');
}

/**
 * Setup the primary and secondary collections.
 */
let primary = replTest.getPrimary();
setLog(primary.getDB("test"));
const insertColl = primary.getDB("test").foo;
const cRecords = 50;
for (let i = 0; i < cRecords; ++i) {
    // We'll be using a unique 'key' field for group & lookup, but we cannot use '_id' for this,
    // because '_id' is indexed and would trigger Indexed Loop Join instead of Hash Join.
    assert.commandWorked(insertColl.insert({key: i, string: "test test test"}));
}

let secondary = replTest.getSecondary();
setLog(secondary.getDB("test"));
const readColl = secondary.getDB("test").foo;

/**
 * Test spilling of $group, when explicitly run on a secondary.
 */
(function testGroupSpilling() {
    if (!checkSBEEnabled(secondary.getDB("test"))) {
        jsTestLog("Skipping test for HashAgg stage: $group lowering into SBE isn't enabled");
        return;
    }

    // Set memory limit so low that HashAgg has to spill all records it processes.
    const oldSetting =
        assert
            .commandWorked(secondary.adminCommand({
                setParameter: 1,
                internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: 1
            }))
            .was;
    try {
        // The pipeline is silly -- because '$key' contains unique values, it will "group" exactly
        // one record into each bucket and push a single '$string' value -- but it allows us to be
        // more deterministic with spilling behaviour: each input record would create a record
        // inside 'HashAgg' and because of the low memory limit, all of them will have to be
        // spilled. For the spilled bytes, sanity test that the number is "reasonably large".
        const pipeline = [{$group: {_id: '$key', s: {$push: '$string'}}}];
        const expectedSpilledRecords = cRecords;
        const expectedSpilledBytesAtLeast = cRecords * 10;

        // Sanity check that the operation fails if cannot use disk and is successful otherwise.
        const aggCommand = {
            pipeline: pipeline,
            allowDiskUse: false,
            readConcern: {level: "majority"},
            writeConcern: {"w": "majority"},
            cursor: {}
        };
        assert.commandFailedWithCode(readColl.runCommand("aggregate", aggCommand),
                                     ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
        let aggOptions = {
            allowDiskUse: true,
            readConcern: {level: "majority"},
            writeConcern: {"w": "majority"}
        };
        const res = readColl.aggregate(pipeline, aggOptions).toArray();
        assert.eq(res.length, cRecords, res);  // the group-by key is unique

        // In SBE also check the statistics for disk usage. Note: 'explain()' doesn't support the
        // 'writeConcern' option so we test spilling on the secondary but without using the concern.
        const explainRes =
            readColl.explain('executionStats').aggregate(pipeline, {allowDiskUse: true});
        const hashAggGroups = getSbePlanStages(explainRes, 'group');
        assert.eq(hashAggGroups.length, 1, explainRes);
        const hashAggGroup = hashAggGroups[0];
        assert(hashAggGroup, explainRes);
        assert(hashAggGroup.hasOwnProperty("usedDisk"), hashAggGroup);
        assert(hashAggGroup.usedDisk, hashAggGroup);
        assert.eq(hashAggGroup.spilledRecords, expectedSpilledRecords, hashAggGroup);
        assert.gte(hashAggGroup.spilledBytesApprox, expectedSpilledBytesAtLeast, hashAggGroup);
    } finally {
        assert.commandWorked(secondary.adminCommand({
            setParameter: 1,
            internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: oldSetting
        }));
    }
})();

/**
 * Test spilling of $lookup when explicitly run on a secondary.
 */
(function testLookupSpillingInSbe() {
    if (!checkSBEEnabled(secondary.getDB("test"))) {
        jsTestLog("Skipping test for HashLookup stage: $lookup lowering into SBE isn't enabled");
        return;
    }

    // Set memory limit so low that HashLookup has to spill all records it processes.
    const oldSetting =
        assert
            .commandWorked(secondary.adminCommand({
                setParameter: 1,
                internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: 1
            }))
            .was;
    try {
        // The pipeline is silly -- because '$key' contains unique values, it will self-join each
        // record with itself and nothing else -- but it allows us to be more deterministic with
        // the spilling behaviour. For the spilled bytes, sanity test that the number is "reasonably
        // large".
        const pipeline = [{
            $lookup:
                {from: readColl.getName(), localField: "key", foreignField: "key", as: "results"}
        }];
        const expectedSpilledRecordsAtLeast = cRecords;
        const expectedSpilledBytesAtLeast = cRecords * 20;

        // Sanity check that the operation is successful. Note: we cannot test the operation to fail
        // with 'allowDiskUse' set to "false" because that would block HashJoin and fall back to NLJ
        // which doesn't spill.
        let aggOptions = {
            allowDiskUse: true,
            readConcern: {level: "majority"},
            writeConcern: {"w": "majority"}
        };
        const res = readColl.aggregate(pipeline, aggOptions).toArray();
        assert.eq(res.length, cRecords);  // the key for self-join is unique

        // In SBE also check the statistics for disk usage. Note: 'explain()' doesn't support the
        // 'writeConcern' option so we test spilling on the secondary but without using the concern.
        const explainRes =
            readColl.explain('executionStats').aggregate(pipeline, {allowDiskUse: true});

        const hLookups = getSbePlanStages(explainRes, 'hash_lookup');
        assert.eq(hLookups.length, 1, explainRes);
        const hLookup = hLookups[0];
        assert(hLookup, explainRes);
        assert(hLookup.hasOwnProperty("usedDisk"), hLookup);
        assert(hLookup.usedDisk, hLookup);
        assert.gte(hLookup.spilledRecords, expectedSpilledRecordsAtLeast, hLookup);
        assert.gte(hLookup.spilledBytesApprox, expectedSpilledBytesAtLeast, hLookup);
    } finally {
        assert.commandWorked(secondary.adminCommand({
            setParameter: 1,
            internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill: oldSetting
        }));
    }
})();

/**
 * Test spilling of $setWindowFields. We only check that the operation is successful. Main tests for
 * $setWindowFields can be found in jstests/aggregation/sources/setWindowFields/spill_to_disk.js.
 */
(function testSetWindowFields() {
    // Test that spilling '$setWindowFields' pipeline on a secondary works with a writeConcern
    // greater than w:1.
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
        assert.commandWorked(
            insertCollWFs.insert({_id: i + smallPartitionSize, val: i, partition: 2}));
    }

    assert.commandWorked(secondary.adminCommand({
        setParameter: 1,
        internalDocumentSourceSetWindowFieldsMaxMemoryBytes: largePartitionSize * avgDocSize + 1
    }));

    const readCollWFs = secondary.getDB("test").bar;

    let pipeline = [
        {
            $setWindowFields: {
                partitionBy: "$partition",
                sortBy: {partition: 1},
                output: {arr: {$push: "$val", window: {documents: [-25, 25]}}}
            }
        },
        {$sort: {_id: 1}}
    ];

    let res = readCollWFs
                  .aggregate(pipeline, {
                      allowDiskUse: true,
                      readConcern: {level: "majority"},
                      writeConcern: {"w": "majority"}
                  })
                  .toArray();
})();

replTest.stopSet();
})();
