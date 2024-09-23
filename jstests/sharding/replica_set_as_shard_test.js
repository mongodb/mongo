/**
 * Checks that a replica set started with --shardsvr can process aggregations on views.
 * @tags: [
 *   requires_persistence,
 *   multiversion_incompatible,
 * ]
 */
(function() {
'use strict';

load("jstests/sharding/libs/sharding_state_test.js");

const kRSName = 'rs';
const kDbName = 'db';
const kCollName = 'coll';
const kViewName = 'collView';
const kNDocs = 25;
const nss = kDbName + '.' + kCollName;

const st = new ShardingTest({shards: 0, mongos: 2});

let rst = new ReplSetTest({name: kRSName, nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let db = primary.getDB(kDbName);

assert.commandWorked(db.createCollection(kCollName));
assert.commandWorked(db.getCollection(kCollName).createIndex({x: 1}));
let coll = db.getCollection(kCollName);
for (let i = 0; i < kNDocs; ++i) {
    assert.commandWorked(coll.insert({x: i}));
}
assert.commandWorked(db.createView(kViewName, kCollName, [{$match: {}}]));

primary.getDB('admin').shutdownServer();
rst.restart(0, {shardsvr: ''});
rst.awaitReplication();

db = rst.getPrimary().getDB(kDbName);
let res = assert.commandWorked(db.runCommand({
    aggregate: kViewName,
    pipeline: [{$match: {}}, {$group: {_id: null, count: {$sum: 1}}}],
    cursor: {}
}));
assert.eq(kNDocs, res.cursor.firstBatch[0].count);

assert.commandWorked(st.s0.adminCommand({addShard: rst.getURL()}));
res = assert.commandWorked(st.s0.getDB(kDbName).runCommand({
    aggregate: kViewName,
    pipeline: [{$match: {}}, {$group: {_id: null, count: {$sum: 1}}}],
    cursor: {}
}));
assert.eq(kNDocs, res.cursor.firstBatch[0].count);
assert.eq(kNDocs, st.s1.getCollection(nss).countDocuments({}));

// Test initial sync.
const staleMongos = st.s1;
const mongos = st.s0;
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: nss, key: {x: 1}}));
ShardingStateTest.addReplSetNode({replSet: rst, serverTypeFlag: 'shardsvr'});
staleMongos.setReadPref('secondary');
assert.eq(kNDocs, staleMongos.getDB(kDbName).getCollection(kCollName).countDocuments({}));

st.stop();
rst.stopSet();
}());
