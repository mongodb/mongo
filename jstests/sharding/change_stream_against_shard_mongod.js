/**
 * Tests that an updateLookup change stream on a sharded collection can be successfully opened
 * and read from on a shard mongoD. Exercises the fix for SERVER-44977.
 * @tags: [
 *   uses_change_streams,
 * ]
 */
(function() {
"use strict";

// Start a new sharded cluster and obtain references to the test DB and collection.
const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongosDB = st.s.getDB(jsTestName());
const mongosColl = mongosDB.test;

const shard0 = st.rs0;
const shard0Coll = shard0.getPrimary().getCollection(mongosColl.getFullName());

// Enable sharding on the the test database and ensure that the primary is shard0.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), shard0.getURL());

// Shard the source collection on {a: 1}. No need to split since it's single-shard.
st.shardColl(mongosColl, {a: 1}, false);

// Open an updateLookup change stream on the collection, against the shard mongoD.
const csCursor = shard0Coll.watch([], {fullDocument: "updateLookup"});

// Write one document onto shard0, then do an op-style update which will require a lookup.
assert.commandWorked(mongosColl.insert({_id: 0, a: -100}));
assert.commandWorked(mongosColl.update({a: -100}, {$set: {updated: true}}));

// Confirm that the stream opened against the shard mongoD sees both events.
const expectedEvents =
    [{op: "insert", doc: {_id: 0, a: -100}}, {op: "update", doc: {_id: 0, a: -100, updated: true}}];
for (let event of expectedEvents) {
    assert.soon(() => csCursor.hasNext());
    const nextDoc = csCursor.next();
    assert.eq(nextDoc.operationType, event.op);
    assert.docEq(event.doc, nextDoc.fullDocument);
}

st.stop();
})();
