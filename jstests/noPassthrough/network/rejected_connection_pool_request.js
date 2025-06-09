/**
 *
 * Tests rejecting connection pool request.
 *
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const databaseName = "my-database";
const collectionName = "my-collection";
const adminDbName = "admin";
const fullCollectionName = databaseName + "." + collectionName;

const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    other: {mongosOptions: {setParameter: {logComponentVerbosity: {network: 5}}}}
});

const mongos = st.s0;
const database = st.s0.getDB(databaseName);
const admin = st.s0.getDB(adminDbName);

function getPendingCount() {
    const stats = admin.runCommand({connPoolStats: 1});
    assert("totalPendingRequests" in stats);
    return stats["totalPendingRequests"];
}

function threadFunc(host, dbName, collName) {
    const mongos = new Mongo(host);
    const database = mongos.getDB(dbName);
    assert.commandWorked(
        database.runCommand({update: collName, updates: [{q: {_id: 1}, u: {name: "Mongo"}}]}));
}

assert.commandWorked(database.runCommand({create: collectionName, writeConcern: {w: "majority"}}));

assert.commandWorked(database.runCommand({find: collectionName}));

const kMaxWaitTimeInSeconds = 10;
const kMaxQueueDepth = 2;
const kMaxPoolSize = 2;
admin.runCommand({setParameter: 1, ShardingTaskExecutorPoolMaxQueueDepth: kMaxQueueDepth});
admin.runCommand({setParameter: 1, ShardingTaskExecutorPoolMaxSize: kMaxPoolSize});

// Make sure both connections are checked out and blocked, so new requests have to enqueue.
const primaryFP = configureFailPoint(st.rs0.getPrimary().getDB("admin"),
                                     "waitAfterCommandFinishesExecution",
                                     {ns: fullCollectionName});

let blockedThreads = [];
for (let i = 0; i < kMaxPoolSize; ++i) {
    let t = new Thread(threadFunc, mongos.host, databaseName, collectionName);
    blockedThreads.push(t);
    t.start();
}

primaryFP.wait(5000, kMaxPoolSize);

// New requests should go and wait in the queue.
for (let i = 0; i < kMaxQueueDepth; ++i) {
    let t = new Thread(threadFunc, mongos.host, databaseName, collectionName);
    blockedThreads.push(t);
    t.start();
}

// Wait until new requests populate the queue.
assert.soon(() => getPendingCount() == kMaxQueueDepth,
            "Failed to wait for the queue size to reach the limit",
            1000 * kMaxWaitTimeInSeconds,
            1000);

const res = database.runCommand({find: collectionName});
assert(res.code == ErrorCodes.PooledConnectionAcquisitionRejected);
assert("errorLabels" in res);
const errorLabels = res["errorLabels"];
assert(errorLabels.includes("SystemOverloadedError"));

primaryFP.off();

for (let i = 0; i < blockedThreads.length; ++i) {
    blockedThreads[i].join();
}

// The same command should work once the connections are available again.
assert.commandWorked(database.runCommand({find: collectionName}));

const stats = admin.runCommand({connPoolStats: 1});
assert("totalRejectedRequests" in stats);
assert.gt(stats["totalRejectedRequests"], 0);
assert("pools" in stats);
const pools = stats["pools"];
assert("NetworkInterfaceTL-TaskExecutorPool-0" in pools);
const poolStats = pools["NetworkInterfaceTL-TaskExecutorPool-0"];
assert("poolRejectedRequests" in poolStats);
assert.gt(poolStats["poolRejectedRequests"], 0);

st.stop();
