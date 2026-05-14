/**
 * SERVER-125921: Sharded time-series collMod must not leave participant critical sections held
 * after aborting on a non-retriable error.
 *
 * Repro:
 *   1. Shard a time-series collection across two shards.
 *   2. Issue a granularity collMod (drives the coordinator into kBlockShards, which engages
 *      _shardsvrParticipantBlock kReadsAndWrites on every participant owning chunks).
 *   3. Inject a non-retriable error on _shardsvrCollModParticipant via failCommand so the
 *      coordinator throws out of Phase::kUpdateShards after the critical section was taken but
 *      before every participant ran the unblock side-effect.
 *   4. Assert (a) the collMod returned an error, (b) resumeMigrations ran (allowMigrations is
 *      not stuck), and (c) NO shard still has a recoverable critical section for the namespace.
 *
 * On the buggy build (no kReleaseCritSec / no _cleanupOnAbort that unblocks), the assertion in
 * step 4(c) is expected to fail: at least one participant's
 * config.collection_critical_sections still has a "blocked: kReadsAndWrites" entry for the ns,
 * and follow-up CRUD on that shard times out.
 *
 * @tags: [
 *   requires_fcv_82,
 *   requires_sharding,
 *   requires_timeseries,
 *   featureFlagShardAuthoritativeCollMetadata_incompatible,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "testDB";
const collName = "tsCollModAbort";
const timeField = "tm";
const metaField = "mt";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});
const mongos = st.s0;
const db = mongos.getDB(dbName);

assert.commandWorked(
    db.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({
    shardCollection: `${dbName}.${collName}`,
    key: {[metaField]: 1},
}));

// Seed a doc on each shard so both shards own chunks for the namespace and thus both will receive
// the participant block.
assert.commandWorked(mongos.adminCommand(
    {split: `${dbName}.${getTimeseriesCollForDDLOps(db, collName)}`, middle: {meta: 0}}));
assert.commandWorked(mongos.adminCommand({
    moveChunk: `${dbName}.${getTimeseriesCollForDDLOps(db, collName)}`,
    find: {meta: 0},
    to: st.shard1.shardName,
}));

const coll = db.getCollection(collName);
assert.commandWorked(coll.insert({[timeField]: new Date(), [metaField]: -1}));
assert.commandWorked(coll.insert({[timeField]: new Date(), [metaField]: 1}));

// Inject a non-retriable error on _shardsvrCollModParticipant on shard1 so the coordinator
// completes kBlockShards (taking critical sections on BOTH shards), advances into kUpdateShards,
// and then throws while processing the participant command on shard1.
//
// ErrorCodes::OperationFailed (96) is non-retriable for the DDL coordinator and is the canonical
// way to drive _isRetriableErrorForDDLCoordinator -> false in this test.
const failPoint = configureFailPoint(st.shard1, "failCommand", {
    errorCode: ErrorCodes.OperationFailed,
    failInternalCommands: true,
    failCommands: ["_shardsvrCollModParticipant"],
});

const collModRes =
    db.runCommand({collMod: collName, timeseries: {granularity: "hours"}, maxTimeMS: 60000});
failPoint.off();

// (a) The collMod returns an error (the non-retriable abort surfaces to the caller).
assert.commandFailed(collModRes, () => `collMod unexpectedly succeeded: ${tojson(collModRes)}`);

// (b) Migrations are resumed (allowMigrations:false should not be left set on config.collections).
assert.soon(
    () => st.config.collections.countDocuments({
              _id: `${dbName}.${getTimeseriesCollForDDLOps(db, collName)}`,
              allowMigrations: false,
          }) === 0,
    "coordinator left allowMigrations:false set after aborting collMod",
);

// (c) SERVER-125921 invariant: NO participant shard may still hold a recoverable critical
//     section for the (raw) time-series namespace. Without the fix, this assertion fails on
//     shard1 (the participant that received the non-retriable error), because the coordinator
//     never sent the unblock command.
const rawNs = `${dbName}.${getTimeseriesCollForDDLOps(db, collName)}`;
for (const shard of [st.shard0, st.shard1]) {
    const stuck =
        shard.getDB("config").collection_critical_sections.find({_id: rawNs}).toArray();
    assert.eq(
        0,
        stuck.length,
        `SERVER-125921: participant ${shard.shardName} left a critical section after collMod abort: ${
            tojson(stuck)}`,
    );
}

// Smoke check: CRUD goes through on both shards after the abort (would otherwise hang on the
// still-blocked participant).
assert.commandWorked(db.runCommand({
    insert: collName,
    documents: [{[timeField]: new Date(), [metaField]: -2}, {[timeField]: new Date(), [metaField]: 2}],
    maxTimeMS: 10000,
}));

st.stop();
