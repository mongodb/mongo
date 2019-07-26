/**
 * Tests restarting the catalog in a sharded cluster on the config server and the shards.
 * @tags: [requires_replication, requires_sharding, requires_majority_read_concern]
 */
(function() {
"use strict";

// Only run this test if the storage engine is "wiredTiger" or "inMemory".
const acceptedStorageEngines = ["wiredTiger", "inMemory"];
const currentStorageEngine = jsTest.options().storageEngine || "wiredTiger";
if (!acceptedStorageEngines.includes(currentStorageEngine)) {
    jsTest.log("Refusing to run restartCatalog test on " + currentStorageEngine +
               " storage engine");
    return;
}

// Helper function for sorting documents in JavaScript.
function sortOn(fieldName) {
    return (doc1, doc2) => {
        return bsonWoCompare({_: doc1[fieldName]}, {_: doc2[fieldName]});
    };
}

const st = new ShardingTest({
    name: "restart_catalog_sharded_cluster",
    mongos: 1,
    config: 1,
    shards: {
        rs: true,
        rs0: {nodes: 1},
        rs1: {nodes: 1},
    },
    other: {
        enableBalancer: false,
        configOptions: {setParameter: "enableTestCommands=1"},
        shardOptions: {setParameter: "enableTestCommands=1"},
    }
});
const mongos = st.s0;
const shard0 = st.shard0;
const shard1 = st.shard1;

const dbName = "drinks";

// Create a sharded collection and distribute chunks amongst the shards.
const coffees = [
    {_id: "americano", price: 1.5},
    {_id: "espresso", price: 2.0},
    {_id: "starbucks", price: 1000.0}
];
const coffeeColl = mongos.getDB(dbName).getCollection("coffee");
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, shard0.shardName);
assert.commandWorked(
    mongos.adminCommand({shardCollection: coffeeColl.getFullName(), key: {price: 1}}));
const splitPoint = 50.0;
assert.commandWorked(
    mongos.adminCommand({split: coffeeColl.getFullName(), middle: {price: splitPoint}}));
for (let coffee of coffees) {
    assert.commandWorked(coffeeColl.insert(coffee, {writeConcern: {w: "majority"}}));
}
assert.commandWorked(mongos.adminCommand({
    moveChunk: coffeeColl.getFullName(),
    find: {price: 1000.0},
    to: shard1.shardName,
    _waitForDelete: true
}));
assert.commandWorked(mongos.adminCommand({
    moveChunk: coffeeColl.getFullName(),
    find: {price: 0.0},
    to: shard0.shardName,
    _waitForDelete: true
}));

// Create an unsharded collection and throw some data in.
const teaColl = mongos.getDB(dbName).getCollection("tea");
const teas =
    [{_id: "darjeeling", price: 2.0}, {_id: "earl gray", price: 1.5}, {_id: "sencha", price: 3.5}];
for (let tea of teas) {
    assert.commandWorked(teaColl.insert(tea, {writeConcern: {w: "majority"}}));
}

// Run queries on both the sharded and unsharded collection.
function assertShardsHaveExpectedData() {
    const dbShard0 = shard0.getDB(dbName);
    const dbShard1 = shard1.getDB(dbName);

    // Assert that we can find all documents in the unsharded collection by either asking
    // mongos, or consulting the primary shard directly.
    assert.eq(teaColl.find().sort({_id: 1}).readConcern("majority").toArray(),
              teas.sort(sortOn("_id")),
              "couldn't find all unsharded data via mongos");
    assert.eq(dbShard0.tea.find().sort({_id: 1}).toArray(),
              teas.sort(sortOn("_id")),
              "couldn't find all unsharded data directly via primary shard");
    assert.eq(teaColl.find().sort({price: 1}).toArray(), teas.sort(sortOn("price")));

    // Assert that we can find all documents in the sharded collection via scatter-gather.
    assert.eq(coffeeColl.find().sort({_id: 1}).readConcern("majority").toArray(),
              coffees.sort(sortOn("_id")),
              "couldn't find all sharded data via mongos scatter-gather");

    // Assert that we can find all documents via a query that targets multiple shards.
    assert.eq(coffeeColl.find({price: {$gt: 0}}).sort({price: 1}).toArray(),
              coffees.sort(sortOn("price")),
              "couldn't find all sharded data via mongos multi-shard targeted query");

    // Assert that we can find all sharded documents on shard0 by shard targeting via mongos,
    // and by consulting shard0 directly.
    const dataShard0 = coffees.filter(drink => drink.price < splitPoint).sort(sortOn("_id"));
    assert.eq(coffeeColl.find({price: {$lt: splitPoint}}).sort({_id: 1}).toArray(),
              dataShard0,
              "couldn't find shard0 data via targeting through mongos");
    jsTest.log(tojson(dbShard0.getCollectionInfos()));
    assert.eq(dbShard0.coffee.find().toArray(),
              dataShard0,
              "couldn't find shard0 data by directly asking shard0");

    // Assert that we can find all sharded documents on shard1 by shard targeting via mongos,
    // and by consulting shard1 directly.
    const dataShard1 = coffees.filter(drink => drink.price >= splitPoint).sort(sortOn("_id"));
    assert.eq(coffeeColl.find({price: {$gte: splitPoint}}).sort({_id: 1}).toArray(),
              dataShard1,
              "couldn't find shard1 data via targeting through mongos");
    assert.eq(dbShard1.coffee.find().toArray(),
              dataShard1,
              "couldn't find shard1 data by directly asking shard1");
}
assertShardsHaveExpectedData();

// Run queries on the metadata stored in the config servers.
function assertConfigServersHaveExpectedData() {
    const configDBViaMongos = mongos.getDB("config");
    const configDBViaConfigSvr = st.config0.getDB("config");
    const projectOnlyShard = {_id: 0, shard: 1};

    // Assert that we can find documents for chunk metadata, both via mongos and by asking the
    // config server primary directly.
    const smallestChunk = {"max.price": splitPoint};
    const smallestChunkShard = {shard: "restart_catalog_sharded_cluster-rs0"};
    assert.eq(configDBViaMongos.chunks.find(smallestChunk, projectOnlyShard).toArray(),
              [smallestChunkShard]);
    assert.eq(configDBViaConfigSvr.chunks.find(smallestChunk, projectOnlyShard).toArray(),
              [smallestChunkShard]);

    const largestChunk = {"min.price": splitPoint};
    const largestChunkShard = {shard: "restart_catalog_sharded_cluster-rs1"};
    assert.eq(configDBViaMongos.chunks.find(largestChunk, projectOnlyShard).toArray(),
              [largestChunkShard]);
    assert.eq(configDBViaConfigSvr.chunks.find(largestChunk, projectOnlyShard).toArray(),
              [largestChunkShard]);
}
assertConfigServersHaveExpectedData();

// Restart the catalog on the config server primary, then assert that both collection data and
// sharding metadata are as expected.
assert.commandWorked(st.config0.getDB("admin").runCommand({restartCatalog: 1}));
assertConfigServersHaveExpectedData();
assertShardsHaveExpectedData();

// Remember what indexes are present, then restart the catalog on all shards via mongos.
const teaIndexesBeforeRestart = teaColl.getIndexes().sort(sortOn("_id"));
const coffeeIndexesBeforeRestart = coffeeColl.getIndexes().sort(sortOn("_id"));
assert.commandWorked(mongos.adminCommand({restartCatalog: 1}));

// Verify that the data in the collections and the metadata have not changed.
assertConfigServersHaveExpectedData();
assertShardsHaveExpectedData();

// Verify that both the sharded and unsharded collection have the same indexes as prior to the
// restart.
const teaIndexesAfterRestart = teaColl.getIndexes().sort(sortOn("_id"));
assert.eq(teaIndexesBeforeRestart, teaIndexesAfterRestart);
const coffeeIndexesAfterRestart = coffeeColl.getIndexes().sort(sortOn("_id"));
assert.eq(coffeeIndexesBeforeRestart, coffeeIndexesAfterRestart);

// Create new indexes on both collections and verify that queries return the same results.
[teaColl, coffeeColl].forEach(coll => {
    assert.commandWorked(coll.createIndex({price: -1}));
    assert.commandWorked(coll.createIndex({price: 1, _id: 1}));
});
assertShardsHaveExpectedData();

// Modify the existing collections.
const validator = {
    price: {$gt: 0}
};
[teaColl, coffeeColl].forEach(coll => {
    assert.commandWorked(coll.runCommand("collMod", {validator: validator}));
    assert.writeErrorWithCode(coll.insert({price: -1}), ErrorCodes.DocumentValidationFailure);
});

// Perform another write, implicitly creating a new collection and database.
const secondTestDB = mongos.getDB("restart_catalog_sharded_cluster_2");
const foodColl = secondTestDB.getCollection("food");
const doc = {
    _id: "apple",
    category: "fruit"
};
assert.commandWorked(foodColl.insert(doc));
assert.commandWorked(foodColl.createIndex({category: 1}));
assert.eq(foodColl.find().toArray(), [doc]);

// Shard the new collection and verify we can find its data again.
assert.commandWorked(mongos.adminCommand({enableSharding: secondTestDB.getName()}));
assert.commandWorked(
    mongos.adminCommand({shardCollection: foodColl.getFullName(), key: {category: 1}}));
assert.eq(foodColl.find().toArray(), [doc]);

// Build a new index on the new collection.
assert.commandWorked(foodColl.createIndex({category: -1}));
assert.eq(foodColl.find().hint({category: -1}).toArray(), [doc]);

st.stop();
}());
