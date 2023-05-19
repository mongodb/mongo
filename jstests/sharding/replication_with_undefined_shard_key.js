// Test for SERVER-31953 where secondaries crash when replicating an oplog entry where the document
// identifier in the oplog entry contains a shard key value that contains an undefined value.
(function() {
"use strict";

const st = new ShardingTest({mongos: 1, shard: 1, rs: {nodes: 2}});
const mongosDB = st.s.getDB("test");
const mongosColl = mongosDB.mycoll;

// Shard the test collection on the "x" field.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
assert.commandWorked(mongosDB.adminCommand({
    shardCollection: mongosColl.getFullName(),
    key: {x: 1},
}));

// Insert a document with a literal undefined value.
assert.commandWorked(mongosColl.insert({x: undefined}));

jsTestLog("Doing writes that generate oplog entries including undefined document key");

assert.commandWorked(mongosColl.update(
    {},
    {$set: {a: 1}},
    {multi: true, writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));
assert.commandWorked(
    mongosColl.remove({}, {writeConcern: {w: 2, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

st.stop();
})();
