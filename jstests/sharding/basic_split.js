/**
 * Perform basic tests for the split command against mongos.
 */
(function() {
"use strict";

var st = new ShardingTest({ shards: 1, other: { chunkSize: 1 }});
var configDB = st.s.getDB('config');

// split on invalid ns.
assert.commandFailed(configDB.adminCommand({ split: 'user', key: { _id: 1 }}));

// split on unsharded collection (db is not sharding enabled).
assert.commandFailed(configDB.adminCommand({ split: 'test.user', key: { _id: 1 }}));

configDB.adminCommand({ enableSharding: 'test' });

// split on unsharded collection (db is sharding enabled).
assert.commandFailed(configDB.adminCommand({ split: 'test.user', key: { _id: 1 }}));

assert.commandWorked(configDB.adminCommand({ shardCollection: 'test.user', key: { _id: 1 }}));

assert.eq(null, configDB.chunks.findOne({ ns: 'test.user', min: { _id: 0 }}));

assert.commandWorked(configDB.adminCommand({ split: 'test.user', middle: { _id: 0 }}));
assert.neq(null, configDB.chunks.findOne({ ns: 'test.user', min: { _id: 0 }}));

// Cannot split on existing chunk boundary.
assert.commandFailed(configDB.adminCommand({ split: 'test.user', middle: { _id: 0 }}));

// Attempt to split on a value that is not the shard key.
assert.commandFailed(configDB.adminCommand({ split: 'test.user', middle: { x: 100 }}));
assert.commandFailed(configDB.adminCommand({ split: 'test.user', find: { x: 100 }}));
assert.commandFailed(configDB.adminCommand({ split: 'test.user',
                                             bounds: [{ x: MinKey }, { x: MaxKey }]}));

// Insert documents large enough to fill up a chunk, but do it directly in the shard in order
// to bypass the auto-split logic.
var kiloDoc = new Array(1024).join('x');
var testDB = st.d0.getDB('test');
var bulk = testDB.user.initializeUnorderedBulkOp();
for (var x = -1200; x < 1200; x++) {
    bulk.insert({ _id: x, val: kiloDoc });
}
assert.writeOK(bulk.execute());

assert.eq(1, configDB.chunks.find({ ns: 'test.user', min: { $gte: { _id: 0 }}}).itcount());

// Errors if bounds do not correspond to existing chunk boundaries.
assert.commandFailed(configDB.adminCommand({ split: 'test.user',
                                             bounds: [{ _id: 0 }, { _id: 1000 }]}));
assert.eq(1, configDB.chunks.find({ ns: 'test.user', min: { $gte: { _id: 0 }}}).itcount());

assert.commandWorked(configDB.adminCommand({ split: 'test.user',
                                             bounds: [{ _id: 0 }, { _id: MaxKey }]}));
assert.gt(configDB.chunks.find({ ns: 'test.user', min: { $gte: { _id: 0 }}}).itcount(), 1);

assert.eq(1, configDB.chunks.find({ ns: 'test.user', min: { $lt: { _id: 0 }}}).itcount());
assert.commandWorked(configDB.adminCommand({ split: 'test.user', find: { _id: -1 }}));
assert.gt(configDB.chunks.find({ ns: 'test.user', min: { $lt: { _id: 0 }}}).itcount(), 1);

//
// Compound Key
//

assert.commandWorked(configDB.adminCommand({ shardCollection: 'test.compound', key: { x: 1, y: 1 }}));

assert.eq(null, configDB.chunks.findOne({ ns: 'test.compound', min: { x: 0, y: 0 }}));
assert.commandWorked(configDB.adminCommand({ split: 'test.compound', middle: { x: 0, y: 0 }}));
assert.neq(null, configDB.chunks.findOne({ ns: 'test.compound', min: { x: 0, y: 0 }}));

// cannot split on existing chunk boundary.
assert.commandFailed(configDB.adminCommand({ split: 'test.compound', middle: { x: 0, y: 0 }}));

bulk = testDB.compound.initializeUnorderedBulkOp();
for (x = -1200; x < 1200; x++) {
    bulk.insert({ x: x, y: x, val: kiloDoc });
}
assert.writeOK(bulk.execute());

assert.eq(1, configDB.chunks.find({ ns: 'test.compound', min: { $gte: { x: 0, y: 0 }}}).itcount());
assert.commandWorked(configDB.adminCommand({ split: 'test.compound',
                                             bounds: [{ x: 0, y: 0 }, { x: MaxKey, y: MaxKey }]}));
assert.gt(configDB.chunks.find({ ns: 'test.compound', min: { $gte: { x: 0, y: 0 }}}).itcount(), 1);

assert.eq(1, configDB.chunks.find({ ns: 'test.compound', min: { $lt: { x: 0, y: 0 }}}).itcount());
assert.commandWorked(configDB.adminCommand({ split: 'test.compound', find: { x: -1, y: -1 }}));
assert.gt(configDB.chunks.find({ ns: 'test.compound', min: { $lt: { x: 0, y: 0 }}}).itcount(), 1);

st.stop();

})();
