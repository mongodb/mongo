/**
 * Tests restarting mongod with 'enableMajorityReadConcern' varying between true and false.
 *
 * @tags: [requires_persistence, requires_replication, requires_majority_read_concern,
 * requires_wiredtiger]
 */
(function() {
"use strict";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

// Insert a document and ensure it is in the stable checkpoint by restarting.
let coll = rst.getPrimary().getDB(dbName)[collName];
assert.commandWorked(coll.insert({_id: 0}, {writeConcern: {w: "majority"}}));
rst.stopSet(undefined, true);
rst.startSet(undefined, true);

// Disable snapshotting on all members of the replica set so that further operations do not
// enter the majority snapshot.
assert.commandWorked(
    rst.getPrimary().adminCommand({configureFailPoint: "disableSnapshotting", mode: "alwaysOn"}));

// Insert a document that will not be in a stable checkpoint.
coll = rst.getPrimary().getDB(dbName)[collName];
assert.commandWorked(coll.insert({_id: 1}));

// Restart the node with enableMajorityReadConcern:false.
rst.stopSet(undefined, true);
rst.startSet({noCleanData: true, enableMajorityReadConcern: "false"});

// Both inserts should be reflected in the data and the oplog.
coll = rst.getPrimary().getDB(dbName)[collName];
assert.eq([{_id: 0}, {_id: 1}], coll.find().sort({_id: 1}).toArray());
let oplog = rst.getPrimary().getDB("local").oplog.rs;
assert.eq(1, oplog.find({o: {_id: 0}}).itcount());
assert.eq(1, oplog.find({o: {_id: 1}}).itcount());

// Restart the node with enableMajorityReadConcern:false without adding any documents.
rst.stopSet(undefined, true);
rst.startSet({noCleanData: true, enableMajorityReadConcern: "false"});

// Both inserts should still be reflected in the data and the oplog.
coll = rst.getPrimary().getDB(dbName)[collName];
assert.eq([{_id: 0}, {_id: 1}], coll.find().sort({_id: 1}).toArray());
oplog = rst.getPrimary().getDB("local").oplog.rs;
assert.eq(1, oplog.find({o: {_id: 0}}).itcount());
assert.eq(1, oplog.find({o: {_id: 1}}).itcount());

// Insert another document.
assert.commandWorked(coll.insert({_id: 2}, {writeConcern: {w: "majority"}}));

// Restart the node with enableMajorityReadConcern:false.
rst.stopSet(undefined, true);
rst.startSet({noCleanData: true, enableMajorityReadConcern: "false"});

// All three inserts should be reflected in the data and the oplog.
coll = rst.getPrimary().getDB(dbName)[collName];
assert.eq([{_id: 0}, {_id: 1}, {_id: 2}], coll.find().sort({_id: 1}).toArray());
oplog = rst.getPrimary().getDB("local").oplog.rs;
assert.eq(1, oplog.find({o: {_id: 0}}).itcount());
assert.eq(1, oplog.find({o: {_id: 1}}).itcount());
assert.eq(1, oplog.find({o: {_id: 2}}).itcount());

// Restart the node with enableMajorityReadConcern:true.
rst.stopSet(undefined, true);
rst.startSet({noCleanData: true, enableMajorityReadConcern: "false"});

// All three inserts should still be reflected in the data and the oplog.
coll = rst.getPrimary().getDB(dbName)[collName];
assert.eq([{_id: 0}, {_id: 1}, {_id: 2}], coll.find().sort({_id: 1}).toArray());
oplog = rst.getPrimary().getDB("local").oplog.rs;
assert.eq(1, oplog.find({o: {_id: 0}}).itcount());
assert.eq(1, oplog.find({o: {_id: 1}}).itcount());
assert.eq(1, oplog.find({o: {_id: 2}}).itcount());

rst.stopSet();
})();
