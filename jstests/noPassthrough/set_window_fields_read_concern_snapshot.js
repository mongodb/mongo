/**
 * Test that $setWindowFields is not supported with readConcern snapshot and in transactions.
 * @tags: [
 *   requires_replication,
 *   uses_transactions,
 *   uses_snapshot_read_concern,
 * ]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const rstPrimary = rst.getPrimary();
const testDB = rstPrimary.getDB(jsTestName() + "_db");
const coll = testDB[jsTestName() + "_coll"];
coll.drop();

assert.commandWorked(coll.insert({_id: 0, val: 0, partition: 1}));

const rsStatus = rst.status();
const lastClusterTime = rsStatus.optimes.lastCommittedOpTime.ts;

let pipeline = [
    {
        $setWindowFields: {
            partitionBy: "$partition",
            sortBy: {partition: 1},
            output: {sum: {$sum: "$val", window: {documents: [-21, 21]}}}
        }
    },
];
let aggregationCommand = {
    aggregate: coll.getName(),
    pipeline: pipeline,
    allowDiskUse: true,
    readConcern: {level: "snapshot", atClusterTime: lastClusterTime},
    cursor: {}
};

// Run outside of a transaction. Fail because read concern snapshot is specified.
assert.commandFailedWithCode(testDB.runCommand(aggregationCommand), ErrorCodes.InvalidOptions);
// Make sure that a $setWindowFields in a subpipeline with readConcern snapshot fails.
const lookupPipeline = [{$lookup: {from: coll.getName(), pipeline: pipeline, as: "newField"}}];
aggregationCommand = {
    aggregate: coll.getName(),
    pipeline: lookupPipeline,
    allowDiskUse: true,
    readConcern: {level: "snapshot", atClusterTime: lastClusterTime},
    cursor: {}
};
assert.commandFailedWithCode(testDB.runCommand(aggregationCommand), ErrorCodes.InvalidOptions);

// Repeat in a transaction.
let session = rstPrimary.startSession();
session.startTransaction({readConcern: {level: "snapshot"}});
const sessionDB = session.getDatabase(testDB.getName());
const sessionColl = sessionDB.getCollection(coll.getName());
aggregationCommand = {
    aggregate: coll.getName(),
    pipeline: pipeline,
    allowDiskUse: true,
    cursor: {},
};
assert.commandFailedWithCode(sessionColl.runCommand(aggregationCommand),
                             ErrorCodes.OperationNotSupportedInTransaction);
// Transaction state is now unusual, abort it and start a new one.
session.abortTransaction();
session.startTransaction({readConcern: {level: "snapshot"}});
// Repeat the subpipeline test in a transaction.
aggregationCommand = {
    aggregate: coll.getName(),
    pipeline: lookupPipeline,
    allowDiskUse: true,
    cursor: {}
};
assert.commandFailedWithCode(sessionColl.runCommand(aggregationCommand),
                             ErrorCodes.OperationNotSupportedInTransaction);
rst.stopSet();
})();
