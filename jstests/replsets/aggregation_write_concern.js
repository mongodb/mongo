/**
 * Confirms that the aggregate command accepts writeConcern and that a read-only aggregation will
 * not wait for the writeConcern specified to be satisfied.
 */
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");  // For stopReplicationOnSecondaries,
                                             // restartReplicationOnSecondaries
const name = "aggregation_write_concern";

// When calling replTest.getPrimary(), the slaveOk bit will be set to true, which will result in
// running all commands with a readPreference of 'secondaryPreferred'. This is problematic in a
// mixed 4.2/4.4 Replica Set cluster as running $out/$merge  with non-primary read preference
// against a cluster in FCV 4.2 is not allowed. As such, setting this test option provides a
// means to ensure that the commands in this test file run with readPreference 'primary'.
TestData.shouldSkipSettingSlaveOk = true;

const replTest = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});

replTest.startSet();
replTest.initiate();

const testDB = replTest.getPrimary().getDB(name);
const collectionName = "test";

// Stop replication and perform a w: 1 write. This will block subsequent 'writeConcern:
// majority' reads if the read command waits on writeConcern.

stopReplicationOnSecondaries(replTest);
assert.commandWorked(
    testDB.runCommand({insert: collectionName, documents: [{_id: 1}], writeConcern: {w: 1}}));

// A read-only aggregation accepts the writeConcern option but does not wait for it.
let res = assert.commandWorked(testDB.runCommand({
    aggregate: collectionName,
    pipeline: [{$match: {_id: 1}}],
    cursor: {},
    writeConcern: {w: "majority"}
}));
assert(res.cursor.firstBatch.length);
assert.eq(res.cursor.firstBatch[0], {_id: 1});

// An aggregation pipeline that writes will block on writeConcern.
assert.commandFailedWithCode(testDB.runCommand({
    aggregate: collectionName,
    pipeline: [{$match: {_id: 1}}, {$out: collectionName + "_out"}],
    cursor: {},
    writeConcern: {w: "majority", wtimeout: 1000}
}),
                             ErrorCodes.WriteConcernFailed);

restartReplicationOnSecondaries(replTest);
replTest.awaitLastOpCommitted();
replTest.stopSet();
})();
