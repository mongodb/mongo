/**
 * Tests that a collection with zone definitions has its zones dropped when it is unsharded.
 * @tags: [assumes_balancer_off]
 */

import {Thread} from "jstests/libs/parallelTester.js";
import {getShards} from "jstests/sharding/libs/sharding_util.js";

const shards = getShards(db);
if (shards.length < 2) {
    jsTestLog("This test requires at least two shards.");
    quit();
}

Random.setRandomSeed();
const dbName = db.getName();

let shard0 = shards[0];
let shard1 = shards[1];

assert.commandWorked(db.adminCommand({enableSharding: dbName, primaryShard: shard0._id}));

assert.commandWorked(db.adminCommand({addShardToZone: shard0._id, zone: "zone1"}));
assert.commandWorked(db.adminCommand({addShardToZone: shard1._id, zone: "zone2"}));

const collName = jsTestName();
const ns = dbName + '.' + collName;

assert.commandWorked(db.createCollection(collName));
assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(db.adminCommand({split: ns, middle: {x: 0}}));

assert.commandWorked(db.adminCommand({moveChunk: ns, find: {x: 0}, to: shard1._id}));

// Define zones for test collection.
assert.commandWorked(
    db.adminCommand({updateZoneKeyRange: ns, min: {x: MinKey}, max: {x: 0}, zone: "zone1"}));
assert.commandWorked(
    db.adminCommand({updateZoneKeyRange: ns, min: {x: 0}, max: {x: MaxKey}, zone: "zone2"}));

for (let i = 0; i < 5000; i++) {
    db.getCollection(ns).insert({x: i});
}

const unshardCollectionThread = function(host, ns) {
    const mongos = new Mongo(host);
    mongos.adminCommand({unshardCollection: ns});
};

let threadForTest = new Thread(unshardCollectionThread, db.getMongo().host, ns);
threadForTest.start();

// Since we sleep for a random interval, there is a chance the unsharding will succeed
// before the abort can happen.
sleep(Random.randInt(10 * 1000));

db.adminCommand({abortUnshardCollection: ns});

threadForTest.join();

let configCollectionDoc = db.getSiblingDB('config').collections.findOne({_id: ns}).key;
const tags = db.getSiblingDB('config').tags.find({ns: ns}).toArray();

// If we successfully unsharded our collection, we should have 0 zones in config.tags.
// If unsharding was unsuccessful, we should retain 2 tags since we specified 2 zones for
// our test collection.
if (configCollectionDoc.hasOwnProperty('_id')) {
    assert.eq(0, tags.length);
} else {
    assert.eq(2, tags.length);
}
