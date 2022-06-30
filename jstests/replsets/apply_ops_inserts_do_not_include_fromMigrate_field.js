/**
 * Tests that insert oplog entries created by applyOps commands do not contain the 'fromMigrate'
 * field. Additionally tests inserts originating from applyOps commands are returned by
 * changeStreams.
 *
 * @tags: [
 *  uses_change_streams,
 *  # Change streams emit events for applyOps without lsid and txnNumber as of SERVER-64972.
 *  multiversion_incompatible,
 * ]
 */
(function() {
'use strict';
load("jstests/libs/change_stream_util.js");  // For ChangeStreamTest.

const rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]});
rst.startSet();
rst.initiate();

function nss(dbName, collName) {
    return `${dbName}.${collName}`;
}

const dbName = 'foo';
const collName = 'coll';
const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const primaryDB = primary.getDB(dbName);

const primaryCST = new ChangeStreamTest(primary.getDB("admin"));
const primaryChangeStream = primaryCST.startWatchingAllChangesForCluster();
const secondaryCST = new ChangeStreamTest(secondary.getDB("admin"));
const secondaryChangeStream = secondaryCST.startWatchingAllChangesForCluster();

primaryDB.createCollection(collName);

// Test non-atomic applyOps inserts.
assert.commandWorked(primaryDB.runCommand(
    {applyOps: [{op: "i", ns: nss(dbName, collName), o: {_id: 0}}], allowAtomic: false}));
assert.commandWorked(primaryDB.runCommand({
    applyOps: [
        {op: "i", ns: nss(dbName, collName), o: {_id: 1}},
        {op: "c", ns: nss(dbName, "$cmd"), o: {create: "other"}}
    ]
}));

// Test non-atomic applyOps upserts. These will be logged as insert oplog entries.
assert.commandWorked(primaryDB.runCommand({
    applyOps: [{op: "u", ns: nss(dbName, collName), o2: {_id: 2}, o: {$v: 2, diff: {u: {x: 2}}}}],
    allowAtomic: false
}));

assert.commandWorked(primaryDB.runCommand({
    applyOps: [
        {op: "u", ns: nss(dbName, collName), o2: {_id: 3}, o: {$v: 2, diff: {u: {x: 3}}}},
        {op: "c", ns: nss(dbName, "$cmd"), o: {create: "other2"}}
    ]
}));

// Test atomic applyOps inserts.
// TODO (SERVER-33182): Remove the atomic applyOps testing once atomic applyOps are removed.
assert.commandWorked(
    primaryDB.runCommand({applyOps: [{op: "i", ns: nss(dbName, collName), o: {_id: 4}}]}));
assert.commandWorked(primaryDB.runCommand({
    applyOps: [
        {op: "i", ns: nss(dbName, collName), o: {_id: 5}},
        {op: "i", ns: nss(dbName, collName), o: {_id: 6}},
    ]
}));
rst.awaitReplication();

assert.eq(7, primaryDB[collName].find().toArray().length);

let expectedCount = 0;
const oplog = rst.getPrimary().getDB("local").getCollection("oplog.rs");
const nonAtomicResults = oplog.find({ns: nss(dbName, collName)}).toArray();
assert.eq(nonAtomicResults.length, 4, nonAtomicResults);
nonAtomicResults.forEach(function(op) {
    // We expect non-atomic applyOps inserts to be picked up by changeStreams.
    const primaryChange = primaryCST.getOneChange(primaryChangeStream);
    assert.eq(primaryChange.documentKey._id, expectedCount, primaryChange);
    const secondaryChange = secondaryCST.getOneChange(secondaryChangeStream);
    assert.eq(secondaryChange.documentKey._id, expectedCount, secondaryChange);

    assert.eq(op.o._id, expectedCount++, nonAtomicResults);
    assert(!op.hasOwnProperty("fromMigrate"), nonAtomicResults);
});

// Atomic applyOps inserts are expected to be picked up by changeStreams.
// We expect the operations from an atomic applyOps command to be nested in an applyOps oplog entry.
const atomicResults = oplog.find({"o.applyOps": {$exists: true}}).toArray();
assert.eq(atomicResults.length, 2, atomicResults);
for (let i = 0; i < atomicResults.length; i++) {
    let ops = atomicResults[i].o.applyOps;
    ops.forEach(function(op) {
        const primaryChange = primaryCST.getOneChange(primaryChangeStream);
        assert.eq(primaryChange.documentKey._id, expectedCount, primaryChange);
        const secondaryChange = secondaryCST.getOneChange(secondaryChangeStream);
        assert.eq(secondaryChange.documentKey._id, expectedCount, secondaryChange);
        assert.eq(op.o._id, expectedCount++, atomicResults);
        assert(!op.hasOwnProperty("fromMigrate"), atomicResults);
    });
}

primaryCST.assertNoChange(primaryChangeStream);
secondaryCST.assertNoChange(secondaryChangeStream);

assert.eq(7, expectedCount);

rst.stopSet();
})();
