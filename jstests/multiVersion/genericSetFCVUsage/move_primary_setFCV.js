
/**
 * Test that `movePrimary` works for databases created under a different FCV
 */
(function() {

"use strict";

let st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 1}});

const mongos = st.s;
const kBeforeDowngradingDbName = 'createdBeforeDowngrading';
const kBeforeUpgradingDbName = 'createdBeforeUpgrading';
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;

const createdBeforeDowngradingDB = mongos.getDB(kBeforeDowngradingDbName);
const createdBeforeUpgradingDB = mongos.getDB(kBeforeUpgradingDbName);

function testMovePrimary(db) {
    const dbName = db.getName();
    const currentPrimary = mongos.getDB('config').databases.findOne({_id: dbName}).primary;
    const newPrimary = currentPrimary == shard0 ? shard1 : shard0;
    assert.eq(db.coll.countDocuments({}), 1);
    assert.commandWorked(mongos.adminCommand({movePrimary: dbName, to: newPrimary}));
    assert.eq(newPrimary, mongos.getDB('config').databases.findOne({_id: dbName}).primary);
    assert.eq(db.coll.countDocuments({}), 1);
}

// Latest FCV
assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Create database `createdBeforeDowngrading` under latest FCV
assert.commandWorked(
    mongos.adminCommand({enableSharding: kBeforeDowngradingDbName, primaryShard: shard0}));
assert.commandWorked(createdBeforeDowngradingDB.coll.insert({_id: 'foo'}));

// Downgrade FCV
assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Make sure movePrimary works for `createdBeforeDowngrading`
testMovePrimary(createdBeforeDowngradingDB);

// Create database `createdBeforeUpgrading` under downgraded FCV
assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(
    mongos.adminCommand({enableSharding: kBeforeUpgradingDbName, primaryShard: shard0}));
assert.commandWorked(createdBeforeUpgradingDB.coll.insert({_id: 'foo'}));

// Upgrade FCV
assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Make sure movePrimary works (again) for `createdBeforeDowngrading`
testMovePrimary(createdBeforeDowngradingDB);

// Make sure movePrimary works for `createdBeforeUpgrading`
testMovePrimary(createdBeforeUpgradingDB);

// Drop databases for next round
assert.commandWorked(createdBeforeDowngradingDB.dropDatabase());
assert.commandWorked(createdBeforeUpgradingDB.dropDatabase());

st.stop();
})();
