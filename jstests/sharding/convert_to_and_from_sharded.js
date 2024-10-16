/**
 * Test that a replica set member can process basic CRUD and DDL operations while switching from
 * being a shardsvr and back to non shardsvr.
 * @tags: [requires_persistence]
 */
/* global retryOnRetryableError */
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

if (jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Delete test
    quit();
}

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

// We only care about non-retryable errors in the background CRUD threads, so retryable errors
// should be retried until they succeed.
TestData.overrideRetryAttempts = 99999;

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
 * Checks that basic CRUD operations work as expected. Expects the collection to have a
 * { _id: _id } document.
 */
const checkBasicCRUD = function(withCollection, _id, isReplSetConnection) {
    const sleepMs = 1;
    const numRetries = 99999;
    const NUM_NODES = 3;

    // A CRUD operation can fail with FailedToSatisfyReadPreference during the stage
    // where the replica set primary is restarted with --shardsvr. This is
    // not a correctness issue and it is safe to retry.
    const additionalCodesToRetry = [];
    if (isReplSetConnection) {
        additionalCodesToRetry.push(ErrorCodes.FailedToSatisfyReadPreference);
    }

    const runWithRetries = (fn) =>
        retryOnRetryableError(fn, numRetries, sleepMs, additionalCodesToRetry);
    const runFindOneWithRetries = (coll, filter) => runWithRetries(() => coll.findOne(filter));

    withCollection((coll) => {
        const doc = runFindOneWithRetries(coll, {_id: _id, y: {$exists: false}});
        assert.neq(null, doc);
    });

    withCollection((coll) => {
        assert.commandWorked(runWithRetries(() => coll.updateOne({_id: _id}, {$set: {y: 2}})));
        assert.eq(2, runFindOneWithRetries(coll, {_id: _id}).y);
    });

    withCollection((coll) => {
        assert.commandWorked(runWithRetries(() => coll.remove({_id: _id}, true /* justOne */)));
        assert.eq(null, runFindOneWithRetries(coll, {_id: _id}));
    });

    withCollection((coll) => {
        assert.commandWorked(
            runWithRetries(() => coll.insert({_id: _id}, {writeConcern: {w: NUM_NODES}})));
        assert.eq(_id, runFindOneWithRetries(coll, {_id: _id})._id);
    });
};

/**
 * Checks that common DDl operations work as expected.
 */
const checkDDLOps = function(withDbs, isReplSetConnection) {
    const DDLDb = "DDL";
    const DDLCollection = "DDLCollection";
    const DDLNs = `${DDLDb}.${DDLCollection}`;
    const sleepMs = 1;
    const numRetries = 99999;

    const assertCommand = (res) => {
        try {
            return assert.commandWorked(res);
        } catch (e) {
            // dropIndexes can fail with a retriable error (InterruptedDueToReplStateChange) after
            // the index drops are complete but before the command is done waiting for a write
            // concern. When the command is automatically retried it will result in an
            // IndexNotFound error which is not a correctness issue.
            if (res._commandObj.dropIndexes && res.code == ErrorCodes.IndexNotFound) {
                return res;
            }

            // renameCollection can fail with a retriable error (InterruptedDueToReplStateChange)
            // after the collection is renamed but before the command is done waiting for a write
            // concern. When the command is automatically retried, it will result in an
            // NamespaceNotFound error which is not a correctness issue.
            if (res._commandObj.renameCollection && res.code == ErrorCodes.NamespaceNotFound) {
                return res;
            }

            // create can fail with a retriable error (InterruptedDueToReplStateChange) after the
            // collection is created but before the command is finished. When the command is
            // automatically retried, it will result in a NamespaceExists error which is not a
            // correctness issue.
            if (res._commandObj.create && res.code == ErrorCodes.NamespaceExists) {
                return res;
            }

            throw e;
        }
    };

    const runDDLCommandWithRetries = (db, cmd, additionalCodesToRetry = []) => {
        // A DDL command can fail with FailedToSatisfyReadPreference during the stage
        // where the replica set primary is restarted with --shardsvr. This is
        // not a correctness issue and it is safe to retry.
        if (isReplSetConnection) {
            additionalCodesToRetry.push(ErrorCodes.FailedToSatisfyReadPreference);
        }
        return retryOnRetryableError(
            () => assertCommand(db.runCommand(cmd)), numRetries, sleepMs, additionalCodesToRetry);
    };

    withDbs((db, _) => {
        jsTestLog("Running create.");
        runDDLCommandWithRetries(db, {create: DDLCollection});
        assert.commandWorked(db[DDLCollection].insertOne({x: 1}));
    });

    withDbs((db, _) => {
        jsTestLog("Running createIndexes.");
        let res = runDDLCommandWithRetries(db, {listIndexes: DDLCollection});
        assert.eq(res["cursor"]["firstBatch"].length, 1, res);
        runDDLCommandWithRetries(db, {
            createIndexes: DDLCollection,
            indexes: [{name: "x_1", key: {x: 1}}, {name: "y_1", key: {y: 1}}]
        });
        res = runDDLCommandWithRetries(db, {listIndexes: DDLCollection});
        assert.eq(res["cursor"]["firstBatch"].length, 3, res);
    });

    withDbs((db, _) => {
        jsTestLog("Running collMod.");
        runDDLCommandWithRetries(db, {collMod: DDLCollection, validator: {x: {$lt: 10}}});
        assert.commandFailedWithCode(db[DDLCollection].insert({x: 11}),
                                     ErrorCodes.DocumentValidationFailure);
    });

    withDbs((db, _) => {
        jsTestLog("Running dropIndexes.");
        runDDLCommandWithRetries(db, {dropIndexes: DDLCollection, index: {y: 1}});
        let res = runDDLCommandWithRetries(db, {listIndexes: DDLCollection});
        assert.eq(res["cursor"]["firstBatch"].length, 2, res);
    });

    withDbs((db, adminDb) => {
        jsTestLog("Running renameCollection.");
        const tempNs = `${DDLDb}.tempName`;
        // rename can fail with ShardNotfound when a shard is being removed, but it is safe to
        // retry.
        let additionalCodesToRetry = [ErrorCodes.ShardNotFound];
        runDDLCommandWithRetries(adminDb,
                                 {renameCollection: DDLNs, to: tempNs, dropTarget: false},
                                 additionalCodesToRetry);
        let res = runDDLCommandWithRetries(db, {listCollections: 1});
        assert.eq(res["cursor"]["firstBatch"][0]["name"], "tempName", res);

        runDDLCommandWithRetries(adminDb,
                                 {renameCollection: tempNs, to: DDLNs, dropTarget: false},
                                 additionalCodesToRetry);
        res = runDDLCommandWithRetries(db, {listCollections: 1});
        assert.eq(res["cursor"]["firstBatch"][0]["name"], DDLCollection, res);
    });

    withDbs((db, _) => {
        jsTestLog("Running drop.");
        runDDLCommandWithRetries(db, {drop: DDLCollection});
        let res = runDDLCommandWithRetries(db, {listCollections: 1});
        assert.eq(res["cursor"]["firstBatch"].length, 0, res);
    });

    withDbs((db, adminDb) => {
        jsTestLog("Running dropDatabase.");
        runDDLCommandWithRetries(db, {dropDatabase: 1});
        let res = runDDLCommandWithRetries(adminDb, {listDatabases: 1});
        assert(!res["databases"].some((database) => database["name"] == DDLDb), res);
    });
};

const checkCRUDThread = function(
    mongosHost, replSetHost, ns, _id, countdownLatch, stage, checkBasicCRUD) {
    const [dbName, collName] = ns.split(".");

    const mongos = new Mongo(mongosHost);
    const mongosSession = mongos.startSession({retryWrites: true, causalConsistency: true});
    const mongosDb = mongosSession.getDatabase(dbName);
    const mongosColl = mongosDb[collName];

    const replSet = new Mongo(replSetHost);
    const replSetSession = replSet.startSession({retryWrites: true, causalConsistency: true});
    const replSetDb = replSetSession.getDatabase(dbName);
    const replSetColl = replSetDb[collName];

    let isReplSetConnection = false;
    const withCollection = (op) => {
        switch (stage.getCount()) {
            case 2:  // Before the replica set is added as a shard.
                op(replSetColl);
                isReplSetConnection = true;
                break;
            case 1:  // After the replica set has been added as a shard.
                // Randomly select mongos or replica set connection to simulate a rolling connection
                // string change.
                if (Math.random() > 0.5) {
                    op(mongosColl);
                } else {
                    op(replSetColl);
                }
                break;
            case 0:  // Right before the second shard is added.
                op(mongosColl);
                break;
        }
    };

    while (countdownLatch.getCount() > 0) {
        checkBasicCRUD(withCollection, _id, isReplSetConnection);
        sleep(1);  // milliseconds.
    }
};

const checkDDLThread = function(mongosHost, replSetHost, countdownLatch, stage, checkDDLOps) {
    const mongos = new Mongo(mongosHost);
    const mongosSession = mongos.startSession({retryWrites: true});
    const mongosDb = mongosSession.getDatabase("DDL");
    const mongosAdminDb = mongosSession.getDatabase("admin");

    const replSet = new Mongo(replSetHost);
    const replSetSession = replSet.startSession({retryWrites: true, causalConsistency: true});
    const replSetDb = replSetSession.getDatabase("DDL");
    const replSetAdminDb = replSetSession.getDatabase("admin");

    while (countdownLatch.getCount() > 0) {
        let db;
        let adminDb;
        let isReplSetConnection = false;
        switch (stage.getCount()) {
            case 2:  // Before the replica set is added as a shard.
                db = replSetDb;
                adminDb = replSetAdminDb;
                isReplSetConnection = true;
                break;
            case 1:  // After the replica set has been added as a shard.
                // TODO SERVER-67835 When replica set endpoint is enabled, change it so that
                // this stage randomly selects between mongos and replica set connection. Also put
                // this code inside withDbs() so that the connection can possibly change for every
                // operation during this stage. The replica set endpoint fixes a bug that allows
                // this to be possible for DDL ops.
                db = mongosDb;
                adminDb = mongosAdminDb;
                break;
            case 0:  // Right before the second shard is added.
                db = mongosDb;
                adminDb = mongosAdminDb;
                break;
        }

        const withDbs = (op) => {
            op(db, adminDb);
        };

        checkDDLOps(withDbs, isReplSetConnection);
        sleep(1);  // milliseconds.
    }
};

const nodeOptions = {
    setParameter: {
        // TODO: SERVER-90040 Remove when PM-3670 (track unsharded collections) fixes the issue
        // with direct to shard operations after addShard has run.
        featureFlagTrackUnshardedCollectionsUponCreation: false
    }
};
const numShards = TestData.configShard ? 1 : 0;

const st = new ShardingTest({
    shards: numShards,
    other: {configOptions: nodeOptions, shardOptions: nodeOptions},
});
const replShard = new ReplSetTest({nodes: NUM_NODES, nodeOptions: nodeOptions});

replShard.startSet({verbose: 1});
replShard.initiate();
let priConn = replShard.getPrimary();

// Insert the initial documents for the background CRUD threads.
assert.commandWorked(priConn.getDB(dbName)[unshardedColl].insert({_id: _id}));
assert.commandWorked(priConn.getDB(dbName)[shardedColl].insert({_id: _id}));

jsTestLog("Starting background CRUD operations.");
// Used to signal to the threads whether to run operations on the replica set, mongos, or both.
const crudStage = new CountDownLatch(2);

const crudStopLatchUnsharded = new CountDownLatch(1);
let crudThreadUnsharded = new Thread(checkCRUDThread,
                                     st.s.host,
                                     replShard.getURL(),
                                     unshardedNs,
                                     _id,
                                     crudStopLatchUnsharded,
                                     crudStage,
                                     checkBasicCRUD);
crudThreadUnsharded.start();

const crudStopLatchSharded = new CountDownLatch(1);
let crudThreadSharded = new Thread(checkCRUDThread,
                                   st.s.host,
                                   replShard.getURL(),
                                   shardedNs,
                                   _id,
                                   crudStopLatchSharded,
                                   crudStage,
                                   checkBasicCRUD);
crudThreadSharded.start();

jsTestLog("Stating background DDL operations.");
// Used to signal to the threads whether to run operations on the replica set, mongos, or both.
const ddlStage = new CountDownLatch(2);
const ddlStopLatch = new CountDownLatch(1);
let ddlThread =
    new Thread(checkDDLThread, st.s.host, replShard.getURL(), ddlStopLatch, ddlStage, checkDDLOps);
ddlThread.start();

jsTestLog("Restarting secondaries with --shardsvr.");
const secondaries = replShard.getSecondaries();
secondaries.forEach(secondary => {
    replShard.restart(secondary, {shardsvr: ''});
});

jsTestLog("Restarting primary with --shardsvr and waiting for new primary.");
let primary = replShard.getPrimary();
primary.adminCommand({replSetStepDown: 600});
replShard.restart(primary, {shardsvr: ''});
replShard.awaitNodesAgreeOnPrimary();

jsTestLog("Adding replica set as shard.");
assert.commandWorked(st.s.adminCommand({addShard: replShard.getURL()}));

jsTestLog(
    "Simulating rolling connection string change by starting background CRUD and DDL ops on mongos.");
ddlStage.countDown();
crudStage.countDown();
sleep(3000);  // Let the background CRUD and DDL operations run for a while.

jsTestLog("Sharding a collection and creating chunks.");
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: shardedNs, key: {_id: 1}}));
for (let x = 0; x < 4; x++) {
    assert.commandWorked(st.s.getDB(dbName)[shardedColl].insert({_id: x}));
    assert.commandWorked(st.s.adminCommand({split: shardedNs, middle: {_id: x}}));
}

jsTestLog("Simulating full connection string change by stopping CRUD and DDL ops on replica set.");
ddlStage.countDown();
crudStage.countDown();

jsTestLog("Adding a second shard.");
const newShard = new ReplSetTest(
    {name: "toRemoveLater", nodes: NUM_NODES, nodeOptions: {shardsvr: "", ...nodeOptions}});
newShard.startSet();
newShard.initiate();
assert.commandWorked(st.s.adminCommand({addShard: newShard.getURL(), name: 'toRemoveLater'}));

jsTestLog("Moving chunks to second shard.");
for (let x = 0; x < 2; x++) {
    assert.commandWorked(
        st.s.adminCommand({moveChunk: shardedNs, find: {_id: x}, to: 'toRemoveLater'}));
}

jsTestLog("Removing second shard.");
// Start the balancer to start draining the chunks.
st.startBalancer();
removeShard(st, 'toRemoveLater');
newShard.stopSet();

jsTestLog("Joining background CRUD ops thread.");
crudStopLatchUnsharded.countDown();
crudThreadUnsharded.join();

crudStopLatchSharded.countDown();
crudThreadSharded.join();

jsTestLog("Joining background DDL ops thread.");
ddlStopLatch.countDown();
ddlThread.join();

st.stop();

jsTest.log('Restarting repl w/o shardsvr');
// Rolling restart is not needed because converting back to replica set can have downtime.
replShard.nodes.forEach(function(node) {
    delete node.fullOptions.shardsvr;
});

replShard.nodes.forEach(node => replShard.restart(node));
replShard.awaitNodesAgreeOnPrimary();

priConn = replShard.getPrimary();
checkBasicCRUD((op) => op(priConn.getDB(dbName)[unshardedColl]), _id);
checkBasicCRUD((op) => op(priConn.getDB(dbName)[shardedColl]), _id);
checkDDLOps((op) => op(priConn.getDB("DDL"), priConn.getDB("admin")));
replShard.stopSet();