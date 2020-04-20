// This test verifies readConcern behavior on a standalone mongod or embedded
// @tags: [requires_majority_read_concern]
(function() {
'use strict';

// For isWiredTiger.
load("jstests/concurrency/fsm_workload_helpers/server_types.js");

var t = db.read_concern;
t.drop();

assert.commandWorked(t.runCommand({insert: "read_concern", documents: [{x: 1}]}));

// Local readConcern succeed.
assert.commandWorked(t.runCommand({find: "read_concern", readConcern: {level: "local"}}),
                     "expected local readConcern to succeed on standalone mongod");

// Available readConcern succeed.
assert.commandWorked(t.runCommand({find: "read_concern", readConcern: {level: "available"}}),
                     "expected available readConcern to succeed on standalone mongod");

var majority_result = t.runCommand({find: "read_concern", readConcern: {level: "majority"}});
if (isWiredTiger(db) || (isEphemeral(db) && !isEphemeralForTest(db))) {
    // Majority readConcern succeed.
    assert.commandWorked(majority_result,
                         "expected majority readConcern to succeed on standalone mongod");
} else {
    // Majority readConcern fail.
    assert.commandFailedWithCode(
        majority_result,
        [ErrorCodes.ReadConcernMajorityNotEnabled, ErrorCodes.NotImplemented],
        "expected majority readConcern to fail on standalone mongod");
}

// Snapshot readConcern fail.
assert.commandFailedWithCode(t.runCommand({find: "read_concern", readConcern: {level: "snapshot"}}),
                             [ErrorCodes.NotAReplicaSet, ErrorCodes.NotImplemented],
                             "expected snapshot readConcern to fail on standalone mongod");

// Standalones don't support any operations with clusterTime.
assert.commandFailedWithCode(
    t.runCommand(
        {find: "read_concern", readConcern: {level: "local", afterClusterTime: Timestamp(0, 1)}}),
    [ErrorCodes.IllegalOperation, ErrorCodes.NotImplemented],
    "expected afterClusterTime read to fail on standalone mongod");
})();
