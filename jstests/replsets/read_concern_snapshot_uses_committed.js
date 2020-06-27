/**
 * Test that non-transaction snapshot reads only see committed data.
 *
 * Non-transaction snapshot reads with atClusterTime (or afterClusterTime) will wait for
 * the majority commit point to move past the atClusterTime (or afterClusterTime) before they can
 * read.
 *
 * @tags: [
 *   requires_fcv_46,
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
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

const committedTimestamp =
    assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{_id: 0}]}))
        .operationTime;
replSet.awaitLastOpCommitted();

stopReplicationOnSecondaries(replSet);

// This document will not replicate.
const nonCommittedTimestamp =
    assert.commandWorked(primaryDB.runCommand({insert: collName, documents: [{_id: 1}]}))
        .operationTime;

for (let db of [primaryDB, secondaryDB]) {
    jsTestLog(`Reading from ${db.getMongo()}`);
    for (let readConcern of [{level: "snapshot"},
                             {level: "snapshot", atClusterTime: committedTimestamp},
                             {level: "snapshot", afterClusterTime: committedTimestamp}]) {
        jsTestLog("Testing committed reads with readConcern " + tojson(readConcern));
        // Only the replicated document is visible to snapshot read, even on primary.
        let res = assert.commandWorked(db.runCommand({find: collName, readConcern: readConcern}));
        assert.sameMembers(res.cursor.firstBatch, [{_id: 0}], tojson(res));

        res = assert.commandWorked(db.runCommand(
            {aggregate: collName, pipeline: [], cursor: {}, readConcern: readConcern}));
        assert.sameMembers(res.cursor.firstBatch, [{_id: 0}], tojson(res));
    }

    for (let readConcern of [{level: "snapshot", atClusterTime: nonCommittedTimestamp},
                             {level: "snapshot", afterClusterTime: nonCommittedTimestamp}]) {
        jsTestLog("Testing non-committed reads with readConcern " + tojson(readConcern));
        // Test that snapshot reads ahead of the committed timestamp are blocked.
        assert.commandFailedWithCode(db.runCommand({
            find: collName,
            readConcern: readConcern,
            maxTimeMS: 1000,
        }),
                                     ErrorCodes.MaxTimeMSExpired);

        assert.commandFailedWithCode(db.runCommand({
            aggregate: collName,
            pipeline: [],
            cursor: {},
            readConcern: readConcern,
            maxTimeMS: 1000
        }),
                                     ErrorCodes.MaxTimeMSExpired);
    }
}

restartReplicationOnSecondaries(replSet);
replSet.stopSet();
}());
