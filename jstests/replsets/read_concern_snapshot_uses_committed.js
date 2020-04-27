/**
 * Test that non-transaction snapshot reads only see committed data.
 *
 * @tags: [requires_majority_read_concern]
 */
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");  // For stopReplicationOnSecondaries.

const replSet = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const dbName = "test";
const collName = "coll";

const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();

const primaryDB = primary.getDB(dbName);
const secondaryDB = secondary.getDB(dbName);

assert.commandWorked(primaryDB[collName].insert({_id: 0}));
replSet.awaitLastOpCommitted();

stopReplicationOnSecondaries(replSet);

// This document will not replicate.
assert.commandWorked(primaryDB[collName].insert({_id: 1}));

for (let db of [primaryDB, secondaryDB]) {
    jsTestLog(`Reading from ${db.getMongo()}`);
    // Only the replicated document is visible to snapshot read, even on primary.
    let res = assert.commandWorked(
        primaryDB.runCommand({find: collName, readConcern: {level: "snapshot"}}));
    print(tojson(res));
    assert.sameMembers(res.cursor.firstBatch, [{_id: 0}]);

    res = assert.commandWorked(primaryDB.runCommand(
        {aggregate: collName, pipeline: [], cursor: {}, readConcern: {level: "snapshot"}}));
    print(tojson(res));
    assert.sameMembers(res.cursor.firstBatch, [{_id: 0}]);
}

restartReplicationOnSecondaries(replSet);
replSet.stopSet();
}());
