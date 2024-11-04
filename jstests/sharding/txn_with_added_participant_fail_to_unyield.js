/**
 * Tests that a transaction will be aborted in the event a transaction participant that adds another
 * participant to the transaction fails to unyield its resources.
 * @tags: [
 *   requires_fcv_80,
 *    # TODO (SERVER-88127): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2});

const dbName = "test";
const localColl = "local";
const foreignColl = "foreign";
const localNs = dbName + "." + localColl;
const foreignNs = dbName + "." + foreignColl;

let shard0 = st.shard0;
let shard1 = st.shard1;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));

// Create an unsharded collection, "local" that lives on shard0.
assert.commandWorked(st.s.getDB(dbName).local.insert({_id: 0, x: 1}));

// Create a sharded collection, "foreign", with one chunk that lives on shard1.
assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({moveChunk: foreignNs, find: {x: 0}, to: shard1.shardName}));
assert.commandWorked(st.s.getDB(dbName).foreign.insert({_id: 1, x: 1}));

const originalMongosMetrics =
    assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
const originalShard0Metrics =
    assert.commandWorked(st.shard0.adminCommand({serverStatus: 1})).transactions;
const originalShard1Metrics =
    assert.commandWorked(st.shard1.adminCommand({serverStatus: 1})).transactions;

// Refresh the routing information for the foreign collection in shard0 before running the checks.
assert.commandWorked(st.shard0.adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));

const session = st.s.startSession();
const sessionDB = session.getDatabase(dbName);
session.startTransaction();

// Set the restoreLocksFail failpoint on shard0, the shard that will add the other shard as a
// participant. This will cause shard0 to fail to unstash its transaction resources due to a
// LockTimeout error.
let fp = configureFailPoint(st.shard0, "restoreLocksFail");

// Run a $lookup where shard0 will add shard1 as an additional participant. The failpoint above
// should cause shard0 to fail to unyield after getting a response from shard1, causing the request
// to fail with a LockTimeout error.
let err = assert.throwsWithCode(() => {
    sessionDB.getCollection(localColl).aggregate(
        [{$lookup: {from: foreignColl, localField: "x", foreignField: "_id", as: "result"}}]);
}, ErrorCodes.LockTimeout);
assert.contains("TransientTransactionError", err.errorLabels, tojson(err));

fp.off();

// Because there was an error with unyielding transaction resources on shard0, shard0 will not have
// the session checked out when it returns an error response to mongos, and so it cannot append
// the added participant to the response. Check the transaction metrics to assert that mongos
// aborted the transaction only on shard0, because it does not know about shard1. Note this is fine
// because shard1 will eventually abort the transaction after TransactionLifeTimeLimitSeconds (or, a
// newer transaction is started on shard1).
const mongosMetrics = assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
assert.gte(mongosMetrics.totalStarted, originalMongosMetrics.totalStarted + 1);
assert.gte(mongosMetrics.currentOpen, 0);
assert.gte(mongosMetrics.totalStarted, originalMongosMetrics.totalStarted + 1);
assert.gte(mongosMetrics.totalContactedParticipants,
           originalMongosMetrics.totalContactedParticipants + 1);

const shard0Metrics = assert.commandWorked(st.shard0.adminCommand({serverStatus: 1})).transactions;
assert.gte(shard0Metrics.totalStarted, originalShard0Metrics.totalStarted + 1);
assert.gte(shard0Metrics.currentOpen, 0);
assert.eq(shard0Metrics.totalAborted, originalShard0Metrics.totalAborted + 1);

const shard1Metrics = assert.commandWorked(st.shard1.adminCommand({serverStatus: 1})).transactions;
assert.gte(shard1Metrics.totalStarted, originalShard1Metrics.totalStarted + 1);
assert.gte(shard1Metrics.currentOpen, originalShard1Metrics.currentOpen + 1);
assert.eq(shard1Metrics.totalAborted, 0);

session.abortTransaction();

st.stop();
