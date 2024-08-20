/**
 * Tests that a replica set member can process basic CRUD operations during and after the conversion
 * to a config shard, as well as after the conversion back to a replica set.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
/* global retryOnRetryableError */
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

// TODO: SERVER-89292 Re-enable test.
quit();

if (jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Delete test.
    quit();
}

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

// We configure the mongo shell to log its retry attempts so there are more diagnostics
// available in case this test ever fails.
TestData.logRetryAttempts = true;

const NUM_NODES = 3;
const dbName = "test";
const shardedColl = "sharded";
const unshardedColl = "unsharded";
const shardedNs = dbName + "." + shardedColl;
const unshardedNs = dbName + "." + unshardedColl;
const _id = "randomId";

/**
 * Checks that basic CRUD operations work as expected.
 */
const checkBasicCRUD = function(coll, _id) {
    const sleepMs = 1;
    const numRetries = 99999;
    const NUM_NODES = 3;

    const retryableFindOne = (coll, filter) =>
        retryOnRetryableError(() => coll.findOne(filter), numRetries, sleepMs);

    const doc = retryableFindOne(coll, {_id: _id, y: {$exists: false}});
    assert.neq(null, doc);

    assert.commandWorked(coll.updateOne({_id: _id}, {$set: {y: 2}}));
    assert.eq(2, retryableFindOne(coll, {_id: _id}).y);

    assert.commandWorked(coll.remove({_id: _id}, true /* justOne */));
    assert.eq(null, retryableFindOne(coll, {_id: _id}));

    assert.commandWorked(coll.insert({_id: _id}, {writeConcern: {w: NUM_NODES}}));
    assert.eq(_id, retryableFindOne(coll, {_id: _id})._id);
};

const checkCRUDThread = function(host, ns, _id, countdownLatch, checkBasicCRUD) {
    const mongo = new Mongo(host);
    const session = mongo.startSession({retryWrites: true});
    const [dbName, collName] = ns.split(".");
    const db = session.getDatabase(dbName);
    while (countdownLatch.getCount() > 0) {
        checkBasicCRUD(db[collName], _id);
        sleep(1);  // milliseconds
    }
};

let replSet = new ReplSetTest({nodes: NUM_NODES, name: "rs_to_config_shard"});
replSet.startSet({});
replSet.initiate();

// Insert the initial documents for the background CRUD threads.
assert.commandWorked(replSet.getPrimary().getDB(dbName)[unshardedColl].insert({_id: _id}));
assert.commandWorked(replSet.getPrimary().getDB(dbName)[shardedColl].insert({_id: _id}));

jsTestLog("Starting background CRUD operations on the replica set.");
// We only care about non-retryable errors in the background thread, so retryable errors should
// be retried until they succeed.
TestData.overrideRetryAttempts = 99999;
const replStopLatch = new CountDownLatch(1);
let replThread =
    new Thread(checkCRUDThread, replSet.getURL(), unshardedNs, _id, replStopLatch, checkBasicCRUD);
replThread.start();

jsTestLog("Restarting secondaries with --configsvr.");
const secondaries = replSet.getSecondaries();
secondaries.forEach(secondary => {
    // TODO SERVER-88880: Remove feature flag.
    replSet.restart(secondary,
                    {configsvr: "", setParameter: {featureFlagTransitionToCatalogShard: true}});
});

jsTestLog("Restarting primary with --configsvr and waiting for new primary.");
let primary = replSet.getPrimary();
// TODO SERVER-88880: Remove feature flag.
replSet.restart(primary,
                {configsvr: "", setParameter: {featureFlagTransitionToCatalogShard: true}});
replSet.awaitNodesAgreeOnPrimary();

jsTestLog("Starting a mongos.");
const replConnStr = replSet.getURL();
// TODO SERVER-88880: Remove feature flag.
const mongos = MongoRunner.runMongos(
    {configdb: replConnStr, setParameter: {featureFlagTransitionToCatalogShard: true}});

jsTestLog("Creating administrative user.");
const mongosAdminDB = mongos.getDB("admin");
mongosAdminDB.createUser({
    user: "admin01",
    pwd: "test",
    roles: [{role: "clusterManager", db: "admin"}, {role: "restore", db: "admin"}]
});
assert(mongosAdminDB.logout());
assert(mongosAdminDB.auth("admin01", "test"));

jsTestLog("Transitioning replica set to config shard.");
assert.commandWorked(mongos.adminCommand({transitionFromDedicatedConfigServer: 1}));

jsTestLog("Simulating connection string change by starting background CRUD ops on mongos.");
replStopLatch.countDown();
replThread.join();

const mongosStopLatchUnsharded = new CountDownLatch(1);
const mongosThreadUnsharded = new Thread(
    checkCRUDThread, mongos.name, unshardedNs, _id, mongosStopLatchUnsharded, checkBasicCRUD);
mongosThreadUnsharded.start();

const mongosStopLatchSharded = new CountDownLatch(1);
const mongosThreadSharded = new Thread(
    checkCRUDThread, mongos.name, shardedNs, _id, mongosStopLatchSharded, checkBasicCRUD);
mongosThreadSharded.start();

jsTestLog("Sharding a collection and creating chunks.");
assert.commandWorked(mongos.adminCommand({shardCollection: shardedNs, key: {_id: 1}}));
for (let x = 0; x < 4; x++) {
    assert.commandWorked(mongos.getDB(dbName)[shardedColl].insert({_id: x}));
    assert.commandWorked(mongos.adminCommand({split: shardedNs, middle: {_id: x}}));
}

jsTestLog("Adding a second shard.");
let newShard = new ReplSetTest({
    nodes: NUM_NODES,
    name: "toRemoveLater",
});
newShard.startSet({shardsvr: ""});
newShard.initiate();
mongos.adminCommand({addShard: newShard.getURL(), name: "toRemoveLater"});

jsTestLog("Moving chunks to second shard.");
for (let x = 0; x < 2; x++) {
    assert.commandWorked(
        mongos.adminCommand({moveChunk: shardedNs, find: {_id: x}, to: "toRemoveLater"}));
}

jsTestLog("Removing second shard.");
assert.commandWorked(mongos.adminCommand({balancerStart: 1, maxTimeMS: 60000}));
removeShard(mongos, "toRemoveLater");
newShard.stopSet();

jsTestLog("Joining background CRUD ops thread on the mongos.");
mongosStopLatchUnsharded.countDown();
mongosThreadUnsharded.join();

mongosStopLatchSharded.countDown();
mongosThreadSharded.join();

MongoRunner.stopMongos(mongos);

jsTestLog("Restarting replica set without --configsvr");
// Rolling restart is not needed because converting back to replica set can have downtime.
replSet.nodes.forEach(function(node) {
    delete node.fullOptions.configsvr;
});
// TODO (SERVER-83433): Add back the test coverage for running db hash check and validation
// on replica set that is fsync locked and has replica set endpoint enabled.
replSet.restart(replSet.nodes, {skipValidation: true});
replSet.awaitNodesAgreeOnPrimary();
primary = replSet.getPrimary();
checkBasicCRUD(primary.getDB(dbName)[shardedColl], _id);
checkBasicCRUD(primary.getDB(dbName)[unshardedColl], _id);

jsTestLog("Shutting down the replica set.");
replSet.stopSet();