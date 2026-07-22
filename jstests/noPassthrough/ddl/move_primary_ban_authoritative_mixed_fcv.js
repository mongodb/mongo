/**
 * Tests that movePrimary is rejected when the donor operates in authoritative-write mode but
 * the recipient shard has not yet entered the transitional FCV during a setFCV upgrade. This
 * prevents stale authoritative shard-catalog entries that would survive a later non-authoritative
 * drop/rename on the recipient.
 *
 * TODO (SERVER-98118): Remove once 9.0 is last-lts.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, config: 1});
const dbName = jsTestName();
const db = st.s.getDB(dbName);
const coll = db.coll;
const collNs = coll.getFullName();

function getShardCatalogEntry(shard) {
    return shard.getDB("config").shard.catalog.collections.findOne({_id: collNs});
}

function assertNoShardCatalogEntry(shard) {
    assert.eq(
        null,
        getShardCatalogEntry(shard),
        `expected no shard.catalog.collections entry on ${shard.name}`,
    );
}

assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: collNs, key: {x: 1}}));

// Fail setFCV so shard0 reaches kUpgrading but shard1 stays at lastLTS.
configureFailPoint(st.rs1.getPrimary(), "failBeforeTransitioning", {}, {times: 1});
assert.commandFailedWithCode(
    st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    6744303,
);

assert.commandFailedWithCode(
    st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}),
    ErrorCodes.ConflictingOperationInProgress,
);

// Recipient must not receive authoritative collection metadata during mixed FCV.
assertNoShardCatalogEntry(st.shard1);
assert.eq(st.shard0.shardName, st.s.getDB("config").databases.findOne({_id: dbName}).primary);

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

// Drop/recreate cycle should work without stale metadata from a rejected movePrimary.
assert(coll.drop());
assertNoShardCatalogEntry(st.shard0);
assertNoShardCatalogEntry(st.shard1);

assert.commandWorked(st.s.adminCommand({shardCollection: collNs, key: {_id: 1}}));
assert(coll.drop());
assertNoShardCatalogEntry(st.shard0);
assertNoShardCatalogEntry(st.shard1);

assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));
assert.eq(st.shard1.shardName, st.s.getDB("config").databases.findOne({_id: dbName}).primary);

st.stop();
