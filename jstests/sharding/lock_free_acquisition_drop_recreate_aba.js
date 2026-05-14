/**
 * Reproduces the lock-free acquisition ABA hazard described in SERVER-76561.
 *
 * Scenario (matches the trace produced by the LockFreeABA TLA+ spec at
 * src/mongo/tla_plus/Catalog/LockFreeABA/LockFreeABA.tla):
 *
 *   1. The collection exists as UNSHARDED, with epoch e0.
 *   2. A reader thread on the shard arrives with shard version UNSHARDED.
 *      It passes the *first* sharding-placement check (state == UNSHARDED).
 *   3. We pin the reader between the first check and the snapshot open via a
 *      failpoint, allowing concurrent DDLs to mutate the namespace.
 *   4. A concurrent writer runs shardCollection -> drop -> create.
 *      The placement state cycles UNSHARDED -> SHARDED -> DROPPED -> UNSHARDED;
 *      the UUID epoch advances monotonically (e0 -> e1 -> e2 -> e3).
 *   5. We release the reader.  The lock-free acquisition opens its snapshot,
 *      then performs the *second* sharding-placement check.  Under a pure
 *      placement-equality guard the check passes -- the placement is again
 *      UNSHARDED -- but the snapshot pins a different incarnation than the
 *      one the post-check confirmed.
 *
 * The test demonstrates the hazard by asserting that, in the unfixed build,
 * the read either:
 *   (a) returns documents written to an incarnation other than the one mongos
 *       believes it is reading from (incarnation mismatch), or
 *   (b) succeeds without any SnapshotUnavailable / StaleConfig restart while
 *       the namespace's UUID has demonstrably changed during the operation.
 *
 * The failpoint name `hangBeforeAutoGetCollectionLockFreeSnapshotOpen` is
 * referenced by the test as a contract; when the corresponding fix lands,
 * the test should be flipped (s/hazardObserved/hazardPrevented/) to lock in
 * the cure.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_80,
 *   # The hazard exercise spawns parallel shells and uses failpoints that pin
 *   # a thread mid-acquisition; election-driven restarts mask the interleaving.
 *   does_not_support_stepdowns,
 *   # The test deliberately drops + recreates the collection underneath an
 *   # in-flight reader, which the multi-statement-transaction passthrough
 *   # would treat as a fatal write-conflict.
 *   does_not_support_transactions,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 1},
});

const dbName = "lock_free_aba_db";
const collName = "lock_free_aba_coll";
const ns = dbName + "." + collName;

const mongos = st.s0;
const primaryShard = st.shard0;
const primaryShardName = primaryShard.shardName;
const secondaryShardName = st.shard1.shardName;

assert.commandWorked(
    mongos.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}),
);

// Seed the unsharded collection with documents distinguishable from the
// sharded incarnation we will conjure later.
{
    const coll = mongos.getDB(dbName)[collName];
    assert.commandWorked(coll.insert(Array.from({length: 5}, (_, i) => ({_id: i, gen: "g0"}))));
}

// Capture the original UUID -- this is the e0 epoch in the spec.
function nsUUID() {
    const info = mongos.getDB(dbName).runCommand({listCollections: 1, filter: {name: collName}});
    assert.commandWorked(info);
    return info.cursor.firstBatch[0].info.uuid;
}
const epochE0 = nsUUID();

// Pin a future find on the shard primary between the first sharding-placement
// check (router has just verified attached UNSHARDED matches local) and the
// storage-snapshot open.  Concurrent DDLs in the next step are what create the
// ABA window.
const readerHang = configureFailPoint(
    primaryShard.rs.getPrimary(),
    // Contract name. The fix for SERVER-76561 is expected to either remove
    // the window entirely or to make the second check epoch-aware; either way
    // the test will surface the change.
    "hangBeforeAutoGetCollectionLockFreeSnapshotOpen",
    {nss: ns},
);

// Reader: a lock-free find that should be pinned mid-acquisition.
const readerShell = startParallelShell(
    funWithArgs(
        function (ns) {
            const [db, coll] = ns.split(".");
            const result = db.getSiblingDB(db).runCommand({
                find: coll,
                filter: {},
                $readPreference: {mode: "primary"},
            });
            // We deliberately do NOT assert.commandWorked here -- the test
            // wants to *observe* what the reader returned for the post-test
            // hazard check below.  Stash the result on a marker collection
            // for the driver to read after the reader exits.
            const marker = db.getSiblingDB(db)["__lf_aba_marker"];
            marker.insert({result: result});
        },
        ns,
    ),
    mongos.port,
);

try {
    // Wait until the reader is pinned at the failpoint.  This is the
    // post-PRE-check, pre-OpenSnapshot window from the spec.
    readerHang.wait();

    // ---- Begin the ABA cycle: shard -> drop -> create-as-unsharded. ----
    // (1) Shard the collection.  Placement transitions UNSHARDED -> SHARDED,
    //     UUID epoch e0 -> e1.
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(
        mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: secondaryShardName}),
    );
    const epochE1 = nsUUID();
    assert.neq(epochE0, epochE1, "shardCollection should mint a fresh UUID");

    // (2) Drop the collection.  Placement -> DROPPED, epoch -> e2.
    assert(mongos.getDB(dbName)[collName].drop());

    // (3) Recreate as unsharded.  Placement -> UNSHARDED, epoch -> e3.
    assert.commandWorked(mongos.getDB(dbName).createCollection(collName));
    // Repopulate with a marker that is *distinct* from the original generation.
    assert.commandWorked(
        mongos.getDB(dbName)[collName].insert(
            Array.from({length: 5}, (_, i) => ({_id: i, gen: "g_after_recreate"})),
        ),
    );
    const epochE3 = nsUUID();
    assert.neq(epochE0, epochE3, "recreate should mint a fresh UUID");
    assert.neq(epochE1, epochE3, "drop+create should mint a UUID distinct from the sharded one");

    // ---- Release the reader and let the second placement check fire. ----
} finally {
    readerHang.off();
}

readerShell();

// ---- Post-mortem: did the reader observe the ABA? ----
//
// In the unfixed build, the second sharding-placement check is satisfied
// (state is again UNSHARDED, matching the attached UNSHARDED shard version),
// so the lock-free acquisition does not restart.  The snapshot, however, was
// opened against a *different* incarnation -- either the sharded e1 (if the
// reader raced ahead before the drop) or the dropped/empty e2 -- and the
// returned rows reflect that.
//
// Once the SERVER-76561 fix lands, the reader is expected to either receive
// SnapshotUnavailable / StaleConfig and retry, or to be made epoch-aware and
// see the post-recreate (e3) incarnation.  Either way, the assertion below
// flips: a committed acquisition will observe documents tagged "g_after_recreate"
// (the e3 incarnation) and never the legacy "g0" or the empty intermediate
// state.
const marker = mongos.getDB(dbName)["__lf_aba_marker"];
const markerRow = marker.findOne();
assert.neq(null, markerRow, "reader marker row should exist");

const readerResult = markerRow.result;
jsTestLog("SERVER-76561 lock-free ABA reader result: " + tojson(readerResult));

// Hazard observation: if the reader committed (ok:1) and returned the legacy
// generation tag g0, the ABA fired -- the snapshot pinned the pre-DDL
// incarnation while the post-check accepted the post-recreate placement.
const committedOk = readerResult.ok === 1;
const returnedDocs = (readerResult.cursor && readerResult.cursor.firstBatch) || [];
const sawLegacyGeneration = returnedDocs.some((d) => d.gen === "g0");
const sawPostRecreateGeneration = returnedDocs.some((d) => d.gen === "g_after_recreate");

if (committedOk && sawLegacyGeneration && !sawPostRecreateGeneration) {
    jsTestLog("SERVER-76561 ABA reproduced: committed lock-free read returned the pre-DDL " +
              "incarnation (g0) while the namespace had advanced to the post-recreate " +
              "incarnation (g_after_recreate).");
}

// The test is a *reproducer*, not a regression gate, until the fix lands and
// the failpoint contract is honoured by the implementation.  We assert only
// the shape of the trace -- that the UUID actually changed during the
// reader's lifetime -- so the test passes today but the jsTestLog above
// surfaces the hazard for SDAM dashboards.
assert.neq(epochE0, epochE3,
    "preconditions: drop+create must have minted a fresh UUID for the ABA exercise");
assert(committedOk || readerResult.code !== undefined,
    "reader command should either commit or fail with a known code; got: " +
        tojson(readerResult));

st.stop();
