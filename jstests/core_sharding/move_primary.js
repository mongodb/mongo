/*
 * Tests basic movePrimary behaviour.
 * @tags: [
 *   featureFlagReshardingImprovements,
 *   featureFlagMoveCollection,
 *   assumes_balancer_off,
 *   requires_fcv_80,
 *   # Stepdown test coverage is already provided by the resharding FSM suites.
 *   does_not_support_stepdowns,
 * ]
 */

function getShardNames(db) {
    return db.adminCommand({listShards: 1}).shards.map(shard => shard._id);
}

// This test requires at least two shards.
const shardNames = getShardNames(db);

if (shardNames.length < 2) {
    jsTestLog("This test requires at least two shards.");
    quit();
}

const dbName = 'test_db';
const collName = 'test_coll_1';
const collNS = dbName + '.' + collName;
const coll = db.getCollection(collNS);

const N = 1000;
const configDb = db.getSiblingDB('config');

var shard0 = shardNames[0];
var shard1 = shardNames[1];

function doInserts(n) {
    jsTestLog("Inserting " + n + " entries.");

    let docs = [];
    for (let i = -(n - 1) / 2; i < n / 2; i++) {
        docs.push({x: i});
    }
    coll.insertMany(docs);
}

function assertPrimaryIs(str) {
    let dbInfo = configDb['databases'].findOne({"_id": dbName});
    assert.eq(dbInfo.primary, str);
}

assert.commandWorked(db.adminCommand({enableSharding: dbName, primaryShard: shard0}));

doInserts(N);
assert.eq(N, coll.countDocuments({}));
assertPrimaryIs(shard0);

jsTestLog("Move primary to another shard and check content.");
assert.commandWorked(db.adminCommand({movePrimary: dbName, to: shard1}));
doInserts(N);
assert.eq(2 * N, coll.countDocuments({}));
assertPrimaryIs(shard1);

jsTestLog("Move primary to the original shard and check content.");
assert.commandWorked(db.adminCommand({movePrimary: dbName, to: shard0}));
doInserts(N);
assert.eq(3 * N, coll.countDocuments({}));
assertPrimaryIs(shard0);

coll.drop();
