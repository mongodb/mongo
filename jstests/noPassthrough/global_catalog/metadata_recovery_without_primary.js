/**
 * Tests that a shard secondary can recover its sharding metadata from the persisted authoritative
 * collections and continue to answer user queries targeting a sharded collection even when it cannot
 * reach a primary of its own replica set.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_persistence,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Some teardown consistency checks talk to the shards while a secondary is still partitioned from
// its primary; skip the ones that would depend on that connectivity.
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckMetadataConsistency = true;

// Each shard is a 3-node replica set so that isolating one secondary still leaves a
// primary + secondary majority in the replica set to avoid triggering elections.
const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 3}, useBridge: true});

const dbName = "test";
const collName = "recovery";
const ns = dbName + "." + collName;

const mongos = st.s0;

// Create a sharded collection and spread its data across two different shards.
assert.commandWorked(
    mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

const coll = mongos.getDB(dbName).getCollection(collName);
assert.commandWorked(coll.insert([{_id: -2}, {_id: -1}, {_id: 1}, {_id: 2}]));

// Sanity check that data really lives on both shards.
assert.eq(2, st.shard0.getCollection(ns).countDocuments({}));
assert.eq(2, st.shard1.getCollection(ns).countDocuments({}));

// Restart the shard replica sets to clear out the filtering metadata. The shard replica sets are
// configured with a very long electionTimeoutMillis, so after a full restart no node spontaneously
// calls an election; explicitly step one up in each shard instead of waiting for a natural election.
jsTest.log.info("Restarting the shards");
st.restartShardRS(0, false /* waitForPrimary */);
st.restartShardRS(1, false /* waitForPrimary */);
for (const rs of [st.rs0, st.rs1]) {
    rs.stepUp(rs.nodes[0], {awaitReplicationBeforeStepUp: false});
}

// For each shard, pick one secondary to isolate from the primary in the replica set, and make it so that
// it's the only secondary that mongos will contact.
const isolatedSecondaries = [];
for (const rs of [st.rs0, st.rs1]) {
    jsTest.log.info("Isolating node");
    const primary = rs.getPrimary();
    const secondaries = rs.getSecondaries();
    const target = secondaries[0];
    const other = secondaries[1];

    jsTest.log.info("Isolating a shard secondary from its primary", {
        replSet: rs.name,
        isolatedSecondary: target.host,
        primary: primary.host,
    });

    target.disconnect(primary);
    primary.disconnect(target);

    // The primary stays reachable from the router (so the router's replica set monitor keeps a healthy
    // topology and doesn't thrash looking for a primary), but the other secondary drops all messages
    // coming from the router.
    other.disconnect(mongos);

    isolatedSecondaries.push({primary, target, other});
}

jsTest.log.info(
    "Verifying queries succeed against the sharded collection while the targeted secondaries cannot reach a primary",
);

const assertDocCount = (filter, expectedCount) => {
    // Immediately after the partition the router's replica set monitor may still consider the now
    // unreachable "other" secondary an eligible target for a "secondary" read and route there; without a
    // maxTimeMS such a read blocks indefinitely waiting for that unreachable node. Bounding each read
    // lets a stuck attempt fail so assert.soon retries until the monitor observes the new topology and
    // routes to the reachable isolated secondary.
    const readTimeoutMS = 3 * 1000;
    assert.soon(() => {
        try {
            const actualCount = coll
                .find(filter)
                .readPref("secondary")
                .maxTimeMS(readTimeoutMS)
                .itcount();
            return actualCount === expectedCount;
        } catch (e) {
            jsTest.log.info(`Retrying query after error`, {
                error: e.toString(),
                filter,
            });
            return false;
        }
    }, "query did not return the expected number of documents");
};

// The broadcast query should return all documents from secondaries.
assertDocCount({}, 4);

// Targeted queries, one for each shard's isolated secondary.
assertDocCount({_id: -1}, 1);
assertDocCount({_id: 1}, 1);

// Heal the partitions before tearing down so that teardown consistency checks can run.
jsTest.log.info("Healing the partitions before teardown");
for (const {primary, target, other} of isolatedSecondaries) {
    jsTest.log.info("Reconnecting nodes", {
        isolatedSecondary: target.host,
        primary: primary.host,
    });
    target.reconnect(primary);
    primary.reconnect(target);
    other.reconnect(mongos);
}

st.restartMongos(0);

st.stop();
