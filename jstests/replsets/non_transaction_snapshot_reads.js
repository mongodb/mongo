/* Tests readConcern level snapshot outside of transactions.
 *
 * TODO(SERVER-46592): This test is multiversion-incompatible in 4.6.  If we use 'requires_fcv_46'
 *                     as the tag for that, removing 'requires_fcv_44' is sufficient.  Otherwise,
 *                     please set the appropriate tag when removing 'requires_fcv_44'
 * @tags: [requires_majority_read_concern, requires_fcv_44, requires_fcv_46]
 */
(function() {
"use strict";

const replSet = new ReplSetTest({nodes: 2});

replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const collName = "coll";
const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
const primaryDB = primary.getDB('test');
const secondaryDB = secondary.getDB('test');

const initialDocuments = [{x: 0}, {x: 1}, {x: 2}, {x: 3}, {x: 4}];
const initialInsertOperationTime =
    assert
        .commandWorked(primaryDB.runCommand(
            {insert: collName, documents: initialDocuments, writeConcern: {w: 2}}))
        .operationTime;

// Do some updates against the initial documents.
for (let i = 0; i < 10; i++) {
    assert.commandWorked(
        primaryDB.getCollection(collName).updateMany({}, {$inc: {x: 1}}, {writeConcern: {w: 2}}));
}

// Tests that updates are not visible to a snapshot read at the initialInsertOperationTime.
jsTestLog("Testing snapshot on primary with atClusterTime " + initialInsertOperationTime);
let cmdRes = assert.commandWorked(primaryDB.runCommand({
    find: collName,
    projection: {_id: 0},
    readConcern: {level: "snapshot", atClusterTime: initialInsertOperationTime},
    // TODO(SERVER-47575): Use a smaller batch size and test getMore.
    batchSize: NumberInt(100)
}));
assert.sameMembers(cmdRes.cursor.firstBatch, initialDocuments, tojson(cmdRes));

jsTestLog("Testing snapshot on secondary with atClusterTime " + initialInsertOperationTime);
cmdRes = assert.commandWorked(secondaryDB.runCommand({
    find: collName,
    projection: {_id: 0},
    readConcern: {level: "snapshot", atClusterTime: initialInsertOperationTime},
    // TODO(SERVER-47575): Use a smaller batch size and test getMore.
    batchSize: NumberInt(100)
}));
assert.sameMembers(cmdRes.cursor.firstBatch, initialDocuments, tojson(cmdRes));

replSet.stopSet();
})();
