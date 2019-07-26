// This test ensures that we respect the value of 'enableMajorityReadConcern' included in TestData.
// @tags: [requires_wiredtiger, requires_replication, requires_majority_read_concern,
// requires_persistence]
(function() {
"use strict";

// Majority reads are enabled by default.
let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let serverStatus = rst.getPrimary().getDB("test").serverStatus();
assert(serverStatus.storageEngine.supportsCommittedReads, tojson(serverStatus));
rst.stopSet();

// Explicitly enable majority reads.
TestData.enableMajorityReadConcern = true;
rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

serverStatus = rst.getPrimary().getDB("test").serverStatus();
assert(serverStatus.storageEngine.supportsCommittedReads, tojson(serverStatus));
rst.stopSet();

// Explicitly disable majority reads.
TestData.enableMajorityReadConcern = false;
rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

serverStatus = rst.getPrimary().getDB("test").serverStatus();
assert(!serverStatus.storageEngine.supportsCommittedReads, tojson(serverStatus));
rst.stopSet();
})();
