/**
 * Verifies that `ClientMetadataPropagationEgressHook` stamps `ifrSenderVersion` and `ifrFlags` on
 * outgoing sharding requests originated by mongos, and that the *original* sender version is
 * preserved across shard-to-shard fan-out hops (SERVER-130136).
 *
 * @tags: [
 *   requires_sharding,
 *   requires_profiling,
 *   # This tests behavior of modern (v9.0) mongos processes. It is not suitable for multiversion
 *   # tests.
 *   requires_fcv_90,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, mongos: 1});
const dbName = jsTestName();
const collName = "coll";
const ns = dbName + "." + collName;

const adminDb = st.s.getDB("admin");
const db = st.s.getDB(dbName);

Random.setRandomSeed();

assert.commandWorked(adminDb.runCommand({enableSharding: dbName}));
assert.commandWorked(adminDb.runCommand({shardCollection: ns, key: {_id: 1}}));

// Insert two documents on distinct shards.
assert.commandWorked(adminDb.runCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(adminDb.runCommand({moveChunk: ns, find: {_id: -1}, to: st.shard0.shardName}));
assert.commandWorked(adminDb.runCommand({moveChunk: ns, find: {_id: 1}, to: st.shard1.shardName}));
assert.commandWorked(db[collName].insertMany([{_id: -1}, {_id: 1}]));

// Enable full profiling on every shard's target database.
for (const shard of [st.rs0.getPrimary(), st.rs1.getPrimary()]) {
    assert.commandWorked(shard.getDB(dbName).setProfilingLevel(2));
}

// Asserts that exactly 'expectedShardCount' of the two shards saw a request tagged with
// 'comment', so a hop that's supposed to reach a specific shard (e.g. a shard-to-shard fan-out)
// can't silently go unobserved while a different shard's unrelated entry keeps the assertion
// passing.
function assertShardsSawIfrSenderVersion(comment, expectedShardCount) {
    let shardsWithEntries = 0;
    let firstObserved = null;
    for (const shard of [st.rs0.getPrimary(), st.rs1.getPrimary()]) {
        const entries = shard
            .getDB(dbName)
            .system.profile.find({"command.comment": comment})
            .toArray();
        if (entries.length === 0) {
            continue;
        }
        shardsWithEntries++;
        for (const entry of entries) {
            // ifrSenderVersion appears at the top-level command (as a generic argument).
            const senderVersion =
                entry.command.ifrSenderVersion ||
                (entry.command.explain && entry.command.explain.ifrSenderVersion);
            assert(
                senderVersion,
                "Expected ifrSenderVersion on shard " +
                    shard.host +
                    ", command: " +
                    tojson(entry.command),
            );
            if (firstObserved === null) {
                firstObserved = senderVersion;
            } else {
                assert.eq(
                    firstObserved,
                    senderVersion,
                    "ifrSenderVersion differed between shards: " +
                        firstObserved +
                        " vs " +
                        senderVersion,
                );
            }
        }
    }
    assert.eq(
        shardsWithEntries,
        expectedShardCount,
        "Expected " +
            expectedShardCount +
            " shard(s) to see comment '" +
            comment +
            "', saw " +
            shardsWithEntries,
    );
    return firstObserved;
}

// Scenario 1: cross-shard aggregate originated from mongos.
{
    const comment = "ifr_egress_agg_" + Random.rand().toString().slice(2, 8);
    assert.commandWorked(
        db.runCommand({
            aggregate: collName,
            pipeline: [{$match: {}}],
            cursor: {},
            comment: comment,
        }),
    );
    // Both shards own a chunk of 'coll', so the unfiltered $match scatters to both directly from
    // mongos.
    const stampedVersion = assertShardsSawIfrSenderVersion(comment, /*expectedShardCount=*/ 2);
    jsTestLog("Aggregate stamped ifrSenderVersion=" + stampedVersion);
}

// Scenario 2: non-aggregate router-driven command (find).
{
    const comment = "ifr_egress_find_" + Random.rand().toString().slice(2, 8);
    assert.commandWorked(
        db.runCommand({
            find: collName,
            filter: {},
            comment: comment,
        }),
    );
    // Both shards own a chunk of 'coll', so the unfiltered find scatters to both directly from
    // mongos.
    assertShardsSawIfrSenderVersion(comment, /*expectedShardCount=*/ 2);
}

// Scenario 3: shard-to-shard fan-out via $lookup on a sharded collection. The downstream shard
// should observe the *mongos*-originating ifrSenderVersion, not an intermediate shard's own.
{
    const otherCollName = "lookup_coll";
    const otherNs = dbName + "." + otherCollName;
    assert.commandWorked(adminDb.runCommand({shardCollection: otherNs, key: {_id: 1}}));
    assert.commandWorked(db[otherCollName].insertMany([{_id: -1}, {_id: 1}]));

    const comment = "ifr_egress_lookup_" + Random.rand().toString().slice(2, 8);
    assert.commandWorked(
        db.runCommand({
            aggregate: collName,
            pipeline: [
                {
                    $lookup: {
                        from: otherCollName,
                        localField: "_id",
                        foreignField: "_id",
                        as: "joined",
                    },
                },
            ],
            cursor: {},
            comment: comment,
        }),
    );
    // 'coll' has one chunk per shard, so both shards receive the aggregate directly from mongos.
    // 'otherCollName' is left with its single default chunk, so whichever of the two shards
    // doesn't own it must fetch matching documents from the other shard to satisfy $lookup — that
    // shard-to-shard hop must carry the original mongos-originating ifrSenderVersion, not a value
    // the intermediate shard stamps itself.
    assertShardsSawIfrSenderVersion(comment, /*expectedShardCount=*/ 2);
}

st.stop();
