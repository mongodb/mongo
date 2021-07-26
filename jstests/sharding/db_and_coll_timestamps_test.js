/**
 * This test checks that the operations that should modify the timestamp of a database or a
 * collection actually do it.
 *
 * For databases, every time a DB is dropped and recreated its timestamp should also be updated
 *
 * For collections, this test verifies that the timestamp is properly updated when sharding a
 * collection, dropping and creating a collection, or refining the sharding key.
 *
 * @tags: [multiversion_incompatible]
 */

(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

function checkTimestampConsistencyInPersistentMetadata(
    dbName, nss, dbTimestampInConfig, collTimestampInConfig) {
    // Checking consistency on local shard collection: config.cache.database
    st.shard0.adminCommand({_flushDatabaseCacheUpdates: dbName, syncFromConfig: true});
    let dbTimestampInShard =
        st.shard0.getDB('config').cache.databases.findOne({_id: dbName}).version.timestamp;
    assert.neq(null, dbTimestampInShard);
    assert.eq(timestampCmp(dbTimestampInConfig, dbTimestampInShard), 0);

    // Checking consistency on local shard collection: config.cache.collections
    st.shard0.adminCommand({_flushRoutingTableCacheUpdates: nss, syncFromConfig: true});
    let collTimestampInShard =
        st.shard0.getDB('config').cache.collections.findOne({_id: nss}).timestamp;
    assert.neq(null, collTimestampInShard);
    assert.eq(timestampCmp(collTimestampInConfig, collTimestampInShard), 0);
}

const kDbName = 'testdb';
const kCollName = 'coll';
const kNs = kDbName + '.' + kCollName;

var st = new ShardingTest({shards: 1, mongos: 1});

let configDB = st.s.getDB('config');

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));

// Create a sharded collection
assert.commandWorked(st.s.adminCommand({shardCollection: kNs, key: {x: 1}}));

// Check that timestamp is created in ConfigSvr and propagated to the shards.
let db = configDB.databases.findOne({_id: kDbName});
let dbTimestampAfterCreate = db.version.timestamp;
assert.neq(null, dbTimestampAfterCreate);

let coll = configDB.collections.findOne({_id: kNs});
let collTimestampAfterCreate = coll.timestamp;
assert.neq(null, collTimestampAfterCreate);

checkTimestampConsistencyInPersistentMetadata(
    kDbName, kNs, dbTimestampAfterCreate, collTimestampAfterCreate);

// Drop the database and implicitly its collections. Create again both and check their timestamps.
st.s.getDB(kDbName).dropDatabase();
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: kNs, key: {x: 1}}));

db = configDB.databases.findOne({_id: kDbName});
let dbTimestampAfterDropDB = db.version.timestamp;
assert.neq(null, dbTimestampAfterDropDB);
assert.eq(timestampCmp(dbTimestampAfterDropDB, dbTimestampAfterCreate), 1);

coll = configDB.collections.findOne({_id: kNs});
let collTimestampAfterDropDB = coll.timestamp;
assert.neq(null, collTimestampAfterDropDB);
assert.eq(timestampCmp(collTimestampAfterDropDB, collTimestampAfterCreate), 1);

checkTimestampConsistencyInPersistentMetadata(
    kDbName, kNs, dbTimestampAfterDropDB, collTimestampAfterDropDB);

// Drop the collection and create it again. Collection timestamp should then be updated.
st.s.getDB(kDbName).coll.drop();
assert.commandWorked(st.s.adminCommand({shardCollection: kNs, key: {x: 1}}));

db = configDB.databases.findOne({_id: kDbName});
let dbTimestampAfterDropColl = db.version.timestamp;
assert.neq(null, dbTimestampAfterDropColl);
assert.eq(timestampCmp(dbTimestampAfterDropColl, dbTimestampAfterDropDB), 0);

coll = configDB.collections.findOne({_id: kNs});
let collTimestampAfterDropColl = coll.timestamp;
assert.neq(null, collTimestampAfterDropColl);
assert.eq(timestampCmp(collTimestampAfterDropColl, collTimestampAfterCreate), 1);

checkTimestampConsistencyInPersistentMetadata(
    kDbName, kNs, dbTimestampAfterDropColl, collTimestampAfterDropColl);

// Refine sharding key. Collection timestamp should then be updated.
assert.commandWorked(st.s.getCollection(kNs).createIndex({x: 1, y: 1}));
assert.commandWorked(st.s.adminCommand({refineCollectionShardKey: kNs, key: {x: 1, y: 1}}));

db = configDB.databases.findOne({_id: kDbName});
let dbTimestampAfterRefine = db.version.timestamp;
assert.neq(null, dbTimestampAfterRefine);
assert.eq(timestampCmp(dbTimestampAfterRefine, dbTimestampAfterDropColl), 0);

coll = configDB.collections.findOne({_id: kNs});
let collTimestampAfterRefine = coll.timestamp;
assert.neq(null, collTimestampAfterRefine);
assert.eq(timestampCmp(collTimestampAfterRefine, collTimestampAfterDropColl), 1);

checkTimestampConsistencyInPersistentMetadata(
    kDbName, kNs, dbTimestampAfterRefine, collTimestampAfterRefine);

st.stop();
})();
