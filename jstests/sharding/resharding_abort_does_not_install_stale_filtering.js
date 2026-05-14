/**
 * Repro for the SERVER-90810 race:
 *   On resharding abort, the recipient drops the temp collection, clears its in-memory filtering
 *   metadata, and asynchronously refreshes it -- then the coordinator deletes the coordinator
 *   document. If a concurrent DDL (e.g. createIndexes / refine on the source) bumps the
 *   authoritative placement version between the async refresh's snapshot and its install, the
 *   recipient ends up with cached filtering metadata that is stale relative to DDLs that ran
 *   during the reshard window.
 *
 * The test:
 *   1. Starts a reshardCollection on a sharded collection with seeded data so cloning actually
 *      runs.
 *   2. Pauses every recipient shard mid-cloning so the abort flow is exercised cleanly.
 *   3. Pauses the coordinator just before it enters the error flow.
 *   4. Issues abortReshardCollection in a parallel shell.
 *   5. While both pauses are held, runs a placement-bumping DDL on the source collection
 *      (createIndexes routed through mongos -- bumps the source collection's shard version /
 *      indexVersion through the standard sharded-DDL path).
 *   6. Releases the coordinator and recipient failpoints; the abort flow completes and the
 *      recipient's async filtering-metadata refresh lands.
 *   7. Asserts (a) the recipient retains no stale filtering metadata for the temporary
 *      resharding namespace, and (b) the recipient's view of the source ns is at least as new
 *      as the authoritative state at the moment the DDL landed.
 *
 * @tags: [
 *   requires_fcv_80,
 *   assumes_balancer_off,
 *   uses_atclustertime,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 1}});

const dbName = "reshardAbortStaleFilteringDb";
const collName = "coll";
const ns = dbName + "." + collName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {oldKey: 1}}));

// Seed data so the cloning phase has something to clone and the recipient genuinely enters
// the cloning state where the failpoint fires.
const sourceColl = st.s.getDB(dbName).getCollection(collName);
const bulk = sourceColl.initializeUnorderedBulkOp();
for (let i = -50; i < 50; i++) {
    bulk.insert({oldKey: i, newKey: -i, payload: "x".repeat(64)});
}
assert.commandWorked(bulk.execute());

// Capture pre-reshard cache entries on every shard for debugging.
for (const rs of [st.rs0, st.rs1]) {
    const entry = rs.getPrimary().getDB("config").getCollection("cache.collections").findOne({_id: ns});
    jsTestLog("Pre-reshard cache entry on " + rs.getPrimary().host + " for source ns: " + tojson(entry));
}

// Pause every shard's primary in the recipient cloning phase. The recipient set is whichever shard
// owns the new-shard-key keyspace; pausing on every shard guarantees we hit the recipient.
const shardFailpoints = [
    configureFailPoint(st.rs0.getPrimary(), "reshardingPauseRecipientDuringCloning"),
    configureFailPoint(st.rs1.getPrimary(), "reshardingPauseRecipientDuringCloning"),
];

jsTestLog("Starting reshardCollection in background");
const reshardThread = startParallelShell(
    funWithArgs(function (ns) {
        assert.commandFailedWithCode(
            db.adminCommand({reshardCollection: ns, key: {newKey: 1}, numInitialChunks: 1}),
            ErrorCodes.ReshardCollectionAborted,
        );
    }, ns),
    st.s.port,
);

// Wait until at least one recipient hits the failpoint. The recipient set is implementation-defined
// based on chunk placement under the new shard key, so we tolerate either shard being the one to
// hit it. `waitWithTimeout` returns a boolean rather than throwing on timeout.
let recipientPrimary = null;
assert.soon(
    () => {
        for (const fp of shardFailpoints) {
            if (fp.waitWithTimeout(1000)) {
                recipientPrimary = fp.conn;
                return true;
            }
        }
        return false;
    },
    "No recipient shard hit reshardingPauseRecipientDuringCloning within timeout",
    5 * 60 * 1000,
);
jsTestLog("Recipient paused at cloning failpoint on " + recipientPrimary.host);

// Capture the temporary resharding namespace. While the recipient is paused mid-cloning, its
// config.cache.collections holds an entry for `<db>.system.resharding.<uuid>` -- this is exactly
// the entry the bug leaves stale.
let tempReshardingNs;
assert.soon(
    () => {
        const cacheEntries = recipientPrimary.getDB("config")
                                 .getCollection("cache.collections")
                                 .find({_id: new RegExp("^" + dbName + "\\.system\\.resharding\\.")})
                                 .toArray();
        if (cacheEntries.length === 0) {
            return false;
        }
        tempReshardingNs = cacheEntries[0]._id;
        return true;
    },
    "Recipient never installed filtering metadata for the temporary resharding namespace",
    2 * 60 * 1000,
);
jsTestLog("Temporary resharding namespace on recipient: " + tempReshardingNs);

// Pause the coordinator just before the error flow starts so the cleanup of the coordinator
// document is deterministically deferred until after our concurrent DDL has landed. This is the
// same failpoint resharding_abort_command.js uses to coordinate the same kind of race window.
const pauseCoordinatorBeforeErrorFlow = configureFailPoint(
    st.configRS.getPrimary(),
    "reshardingPauseCoordinatorBeforeStartingErrorFlow",
);

jsTestLog("Issuing abortReshardCollection in background");
const abortThread = startParallelShell(
    funWithArgs(function (ns) {
        assert.commandWorked(db.adminCommand({abortReshardCollection: ns}));
    }, ns),
    st.s.port,
);

pauseCoordinatorBeforeErrorFlow.wait();

// === Concurrent DDL during the reshard abort window ===
//
// While both failpoints are held (coordinator just before error flow, recipient mid-cloning) we
// run a placement-bumping DDL against the source collection. The DDL must land before the
// recipient's async filtering-metadata refresh captures its snapshot -- which is exactly the
// window the SERVER-90810 race opens. After the failpoints are released the refresh runs to
// completion; the post-abort assertions verify the refresh did not install a stale snapshot.
jsTestLog("Running concurrent createIndexes DDL on source collection during abort window");
assert.commandWorked(st.s.getDB(dbName).runCommand({
    createIndexes: collName,
    indexes: [{key: {newKey: 1, oldKey: 1}, name: "concurrent_ddl_during_abort"}],
}));

// Authoritative metadata for the source collection after the DDL landed. This is the floor that
// the recipient's post-abort filtering metadata for `ns` must respect.
const postDDLConfigEntry =
    st.configRS.getPrimary().getDB("config").getCollection("collections").findOne({_id: ns});
assert.neq(null, postDDLConfigEntry,
           "Source collection metadata disappeared from config.collections during abort");
jsTestLog("Authoritative config metadata post-DDL: " + tojson(postDDLConfigEntry));

// Release the coordinator and recipient failpoints so the abort flow completes.
pauseCoordinatorBeforeErrorFlow.off();
for (const fp of shardFailpoints) {
    fp.off();
}

jsTestLog("Joining background threads");
abortThread();
reshardThread();

// === Post-abort assertions ===

// (1) The recipient's cached filtering metadata for the *temporary* resharding namespace must NOT
//     be retained as a stale entry. SERVER-90810 leaves the recipient with cached filtering
//     information for the temporary namespace even though the coordinator has cleaned up its
//     authoritative metadata. Either of the following are correct end states:
//       (a) No cache entry for the temp namespace remains.
//       (b) A cache entry remains but its timestamp is at least as new as the post-DDL
//           authoritative timestamp for the source collection -- proving the refresh did not
//           install a snapshot taken before the DDL landed.
//     The bug surfaces as a cache entry whose timestamp is older than the post-DDL authoritative
//     timestamp.
assert.soon(
    () => {
        const tempCacheEntry =
            recipientPrimary.getDB("config")
                .getCollection("cache.collections")
                .findOne({_id: tempReshardingNs});
        if (tempCacheEntry === null) {
            return true;
        }
        const tempTs = tempCacheEntry.timestamp;
        const sourceTs = postDDLConfigEntry.timestamp;
        if (tempTs === undefined || sourceTs === undefined) {
            return false;
        }
        return (tempTs.t > sourceTs.t ||
                (tempTs.t === sourceTs.t && tempTs.i >= sourceTs.i));
    },
    () => {
        const tempCacheEntry = recipientPrimary.getDB("config")
                                   .getCollection("cache.collections")
                                   .findOne({_id: tempReshardingNs});
        return ("Recipient retained stale filtering metadata for temp resharding namespace "
                + tempReshardingNs
                + " after abort. Recipient cache entry: " + tojson(tempCacheEntry)
                + "; authoritative source-collection metadata at DDL completion: "
                + tojson(postDDLConfigEntry));
    },
    2 * 60 * 1000,
);

// (2) The recipient's view of the *source* collection must also be at least as new as the
//     post-DDL authoritative state. Force a refresh first to give the recipient a chance to
//     observe the latest authoritative state; if it can't, the recipient is wedged on stale
//     filtering metadata.
assert.soon(
    () => {
        const flushResult = recipientPrimary.adminCommand(
            {_flushRoutingTableCacheUpdates: ns, syncFromConfig: true});
        return flushResult.ok === 1;
    },
    "Recipient could not flush routing table cache updates for source ns after abort",
    2 * 60 * 1000,
);

const postAbortSourceCacheEntry =
    recipientPrimary.getDB("config").getCollection("cache.collections").findOne({_id: ns});
assert.neq(
    null, postAbortSourceCacheEntry,
    "Recipient has no cache entry for the source ns after abort + post-abort flush");
jsTestLog("Post-abort recipient cache entry for source ns: " + tojson(postAbortSourceCacheEntry));

const recipientTs = postAbortSourceCacheEntry.timestamp;
const authoritativeTs = postDDLConfigEntry.timestamp;
assert(
    recipientTs.t > authoritativeTs.t
        || (recipientTs.t === authoritativeTs.t && recipientTs.i >= authoritativeTs.i),
    "Recipient's cached filtering metadata for source ns is older than authoritative metadata "
        + "after DDL during reshard abort window. Recipient: " + tojson(recipientTs)
        + "; authoritative: " + tojson(authoritativeTs));

st.stop();
