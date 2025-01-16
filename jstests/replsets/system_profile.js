// This tests that metadata commands run against the system.profile collection are not replicated
// to the secondary.
//
// @tags: [
//   # The test queries the system.profile collection so it is not compatible with initial sync
//   # since an initial sync may insert unexpected operations into the profile collection.
//   queries_system_profile_collection
// ]

import {ReplSetTest} from "jstests/libs/replsettest.js";

var rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
rst.awaitReplication();

// filter out noop writes
var getLatestOp = function() {
    return primaryDB.getSiblingDB('local')
        .oplog.rs.find({op: {$ne: 'n'}})
        .sort({$natural: -1})
        .limit(1)
        .next();
};

var primaryDB = rst.getPrimary().getDB('test');
assert.commandWorked(primaryDB.foo.insert({}));
var op = getLatestOp();

// Enable profiling on the primary
assert.commandWorked(primaryDB.runCommand({profile: 2}));
assert.eq(op, getLatestOp(), "oplog entry created when profile was enabled");
assert.commandWorked(primaryDB.foo.insert({}));
op = getLatestOp();
assert.commandWorked(primaryDB.runCommand({profile: 0}));
assert.eq(op, getLatestOp(), "oplog entry created when profile was disabled");

// dropCollection
assert(primaryDB.system.profile.drop());
assert.eq(op, getLatestOp(), "oplog entry created when system.profile was dropped");

assert.commandWorked(primaryDB.createCollection("system.profile", {capped: true, size: 1000}));
assert.eq(op, getLatestOp(), "oplog entry created when system.profile was created");
assert.commandWorked(primaryDB.runCommand({profile: 2}));
assert.commandWorked(primaryDB.foo.insert({}));
op = getLatestOp();
assert.commandWorked(primaryDB.runCommand({profile: 0}));

assert(primaryDB.system.profile.drop());
rst.stopSet();
