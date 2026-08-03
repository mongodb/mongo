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
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
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

// movePrimary must also be rejected when the recipient shard is mid-upgrade. Here the whole cluster
// starts at lastLTS and an upgrade is paused so the donor (shard0) has completed to latest (fully
// authoritative) while the recipient (shard1) is parked in the transitional kUpgrading FCV. Since
// the recipient is still in an FCV transition, movePrimary must be refused.
assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
);
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

// Pause the recipient (shard1) inside _runUpgrade, before it persists the fully-upgraded FCV, so it
// stays at kUpgrading while the donor (shard0) completes the upgrade to latest.
const hangUpgradeFp = configureFailPoint(st.rs1.getPrimary(), "hangWhileUpgrading");
const awaitUpgrade = startParallelShell(
    funWithArgs(function (targetFCV) {
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}),
        );
    }, latestFCV),
    st.s.port,
);
hangUpgradeFp.wait();

try {
    assert.commandFailedWithCode(
        st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}),
        ErrorCodes.ConflictingOperationInProgress,
    );
    assert.eq(st.shard0.shardName, st.s.getDB("config").databases.findOne({_id: dbName}).primary);
} finally {
    hangUpgradeFp.off();
    awaitUpgrade();
}

// Once the upgrade completes, movePrimary is allowed again.
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));
assert.eq(st.shard1.shardName, st.s.getDB("config").databases.findOne({_id: dbName}).primary);

st.stop();
