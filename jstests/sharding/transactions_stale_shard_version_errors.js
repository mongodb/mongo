// Tests mongos behavior on stale shard version errors received in a transaction.
//
// @tags: [
//  requires_sharding,
//  uses_transactions,
//  uses_multi_shard_transaction,
//  assumes_balancer_off,
// ]
import "jstests/multiVersion/libs/verify_versions.js";

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {
    assertNoSuchTransactionOnAllShards,
    disableStaleVersionAndSnapshotRetriesWithinTransactions,
    enableStaleVersionAndSnapshotRetriesWithinTransactions,
    kShardOptionsForDisabledStaleShardVersionRetries,
} from "jstests/sharding/libs/sharded_transactions_helpers.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

function expectChunks(st, ns, chunks) {
    for (let i = 0; i < chunks.length; i++) {
        assert.eq(
            chunks[i],
            findChunksUtil.countChunksForNs(st.s.getDB("config"), ns, {
                shard: st["shard" + i].shardName,
            }),
            "unexpected number of chunks on shard " + i,
        );
    }
}

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

// Disable checking for index consistency to ensure that the config server doesn't trigger a
// StaleShardVersion exception on shards and cause them to refresh their sharding metadata.
const configOptions = {
    setParameter: {enableShardedIndexConsistencyCheck: false},
};

const st = new ShardingTest({
    shards: 3,
    mongos: 2,
    other: {
        rsOptions: kShardOptionsForDisabledStaleShardVersionRetries,
        configOptions: configOptions,
        enableBalancer: false,
    },
});

enableStaleVersionAndSnapshotRetriesWithinTransactions(st);

// A shard can never be ignorant of its own metadata under authoritative shards, so staleness is
// induced on the transaction's router (txnRouter) instead: migrations are performed through the
// other router (migrateRouter), leaving txnRouter with a stale routing table.
const txnRouter = st.s;
const migrateRouter = st.s1;

// Every collection in this test is split once at {_id: 0}, giving chunks [MinKey, 0) and
// [0, MaxKey). Returns the bounds of the chunk that owns `shardKeyValue` and of its sibling.
function chunkBoundsForValue(shardKeyValue) {
    const belowSplit = [{_id: MinKey}, {_id: 0}];
    const aboveSplit = [{_id: 0}, {_id: MaxKey}];
    return shardKeyValue < 0
        ? {target: belowSplit, sibling: aboveSplit}
        : {target: aboveSplit, sibling: belowSplit};
}

// Maps a shard name back to its ShardingTest shard connection.
const shardByName = {};
for (let i = 0; i < 3; i++) {
    shardByName[st["shard" + i].shardName] = st["shard" + i];
}

// Placement-stale: move the chunk owning `shardKeyValue` to `toShard`, so txnRouter still routes to
// its previous owner. The statement targets that previous owner, gets a StaleConfig, and retargets.
function runWithPlacementStaleRouter(ns, shardKeyValue, toShard, runStaleOperation) {
    return ShardVersioningUtil.runOperationOnStaleRouterAfterMoveChunk({
        migrateRouter,
        staleRouter: txnRouter,
        ns,
        bounds: chunkBoundsForValue(shardKeyValue).target,
        toShard,
        runStaleOperation,
    });
}

// Version-stale: move the sibling of `shardKeyValue`'s chunk onto that chunk's current owner,
// bumping the owner's placement version while the chunk stays put. The statement still targets that
// owner (same participant) but with a stale version, gets a StaleConfig, and refreshes.
function runWithVersionStaleRouter(ns, shardKeyValue, runStaleOperation) {
    const {target, sibling} = chunkBoundsForValue(shardKeyValue);
    const ownerShardName = findChunksUtil.findOneChunkByNs(migrateRouter.getDB("config"), ns, {
        min: target[0],
    }).shard;
    return ShardVersioningUtil.runOperationOnStaleRouterAfterMoveChunk({
        migrateRouter,
        staleRouter: txnRouter,
        ns,
        bounds: sibling,
        toShard: shardByName[ownerShardName],
        runStaleOperation,
    });
}

// Shard two collections in the same database, each with 2 chunks, [minKey, 0), [0, maxKey),
// with one document each, all on Shard0.

assert.commandWorked(
    txnRouter.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(
    txnRouter.getDB(dbName)[collName].insert({_id: -5}, {writeConcern: {w: "majority"}}),
);
assert.commandWorked(
    txnRouter.getDB(dbName)[collName].insert({_id: 5}, {writeConcern: {w: "majority"}}),
);

assert.commandWorked(txnRouter.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(txnRouter.adminCommand({split: ns, middle: {_id: 0}}));

expectChunks(st, ns, [2, 0, 0]);

const otherCollName = "bar";
const otherNs = dbName + "." + otherCollName;

assert.commandWorked(
    txnRouter.getDB(dbName)[otherCollName].insert({_id: -5}, {writeConcern: {w: "majority"}}),
);
assert.commandWorked(
    txnRouter.getDB(dbName)[otherCollName].insert({_id: 5}, {writeConcern: {w: "majority"}}),
);

assert.commandWorked(txnRouter.adminCommand({shardCollection: otherNs, key: {_id: 1}}));
assert.commandWorked(txnRouter.adminCommand({split: otherNs, middle: {_id: 0}}));

expectChunks(st, otherNs, [2, 0, 0]);

const session = txnRouter.startSession();
const sessionDB = session.getDatabase(dbName);

// Makes the transaction's session causally aware of migrations performed through migrateRouter, so
// the transaction picks a snapshot after a version-bump migration (avoiding a MigrationConflict)
// without refreshing txnRouter's stale routing. Needed before starting a transaction whose statement
// retries on a new participant (a retry re-reads at the transaction's snapshot).
function advanceSessionPastMigrations() {
    const clusterTime = migrateRouter.getClusterTime();
    session.advanceClusterTime(clusterTime);
    session.advanceOperationTime(clusterTime.clusterTime);
}

//
// Stale shard version on first overall command should succeed.
//

session.startTransaction();
// The StaleConfig on the first statement restarts the transaction, which then succeeds on Shard1.
runWithPlacementStaleRouter(ns, 5, st.shard1, () => {
    assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: 5}}));
});
expectChunks(st, ns, [1, 1, 0]);

assert.commandWorked(session.commitTransaction_forTesting());

//
// Stale shard version on second command to a shard should fail.
//

expectChunks(st, ns, [1, 1, 0]);

// Co-locate the other collection's chunk with the first collection's on Shard1, so the second
// statement targets Shard1 — the shard the first statement already made a participant.
assert.commandWorked(
    txnRouter.adminCommand({moveChunk: otherNs, find: {_id: 5}, to: st.shard1.shardName}),
);
expectChunks(st, otherNs, [1, 1, 0]);

session.startTransaction();

// Makes Shard1 a participant (not stale for the first ns).
assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: 5}}));

// The stale statement hits an already-participating shard, so the retry cannot restart the
// transaction and fails with NoSuchTransaction. (It aborts without re-reading at the snapshot, so a
// mid-transaction version-bump migration is fine here.)
let res = runWithVersionStaleRouter(otherNs, 5, () => {
    return assert.commandFailedWithCode(
        sessionDB.runCommand({find: otherCollName, filter: {_id: 5}}),
        ErrorCodes.NoSuchTransaction,
    );
});
assert.eq(res.errorLabels, ["TransientTransactionError"]);

assertNoSuchTransactionOnAllShards(st, session.getSessionId(), session.getTxnNumber_forTesting());
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

expectChunks(st, otherNs, [0, 2, 0]);

//
// Stale shard version on first command to a new shard should succeed.
//

expectChunks(st, ns, [1, 1, 0]);

// Place the other collection's chunk on Shard0, so the second statement targets Shard0 — a shard
// that is not yet a participant.
assert.commandWorked(
    txnRouter.adminCommand({moveChunk: otherNs, find: {_id: 5}, to: st.shard0.shardName}),
);
expectChunks(st, otherNs, [1, 1, 0]);

// The whole transaction runs inside the helper so the version-bump migration precedes it.
runWithVersionStaleRouter(otherNs, 5, () => {
    // The stale statement retries on a new participant, which re-reads at the snapshot, so the
    // snapshot must land after the version-bump migration.
    advanceSessionPastMigrations();
    session.startTransaction();
    // Makes Shard1 a participant (not stale for the first ns).
    assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: 5}}));
    // The stale statement hits Shard0, a new participant, so it is resolved and succeeds.
    assert.commandWorked(sessionDB.runCommand({find: otherCollName, filter: {_id: 5}}));
    assert.commandWorked(session.commitTransaction_forTesting());
});

expectChunks(st, otherNs, [2, 0, 0]);

//
// Stale mongos aborts on old shard.
//

session.startTransaction();

// The stale router targets Shard1 (previous owner), hits a stale version error, restarts the
// transaction, and succeeds on Shard0. The sub-transaction the stale router started on Shard1 must
// be aborted (verified below).
runWithPlacementStaleRouter(ns, 5, st.shard0, () => {
    assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: 5}}));
});
expectChunks(st, ns, [2, 0, 0]);

assert.commandWorked(session.commitTransaction_forTesting());

// Verify there is no in-progress transaction on Shard1.
res = assert.commandFailedWithCode(
    st.rs1
        .getPrimary()
        .getDB(dbName)
        .runCommand({
            find: collName,
            lsid: session.getSessionId(),
            txnNumber: NumberLong(session.getTxnNumber_forTesting()),
            autocommit: false,
        }),
    ErrorCodes.NoSuchTransaction,
);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

//
// More than one stale shard version error.
//

// Position the two chunks on Shard1 and Shard2. Moving [MinKey, 0) from Shard1 to Shard2 below then
// makes both shards stale to txnRouter at once: Shard1 (the donor) loses the chunk txnRouter still
// routes there, and Shard2's (the recipient's) placement version bumps so txnRouter is also stale
// for the [0, MaxKey) chunk that never moved.
assert.commandWorked(
    txnRouter.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard2.shardName}),
);
assert.commandWorked(
    txnRouter.adminCommand({moveChunk: ns, find: {_id: -5}, to: st.shard1.shardName}),
);
expectChunks(st, ns, [0, 1, 1]);

session.startTransaction();

// Targets all shards; both Shard1 and Shard2 are stale on the first statement, so the transaction
// restarts and succeeds.
runWithPlacementStaleRouter(ns, -5, st.shard2, () => {
    assert.commandWorked(sessionDB.runCommand({find: collName}));
});
expectChunks(st, ns, [0, 0, 2]);

assert.commandWorked(session.commitTransaction_forTesting());

//
// Can retry a stale write on the first statement.
//

session.startTransaction();

// The stale write targets Shard2 (previous owner) on the first statement, so the transaction
// restarts and succeeds on Shard1.
runWithPlacementStaleRouter(ns, 5, st.shard1, () => {
    assert.commandWorked(sessionDB.runCommand({insert: collName, documents: [{_id: 6}]}));
});
expectChunks(st, ns, [0, 1, 1]);

assert.commandWorked(session.commitTransaction_forTesting());

//
// A stale write past the first statement aborts the transaction.
//
// Only reads are retried past the first statement (see alwaysRetryableCmds in transaction_router);
// a stale write past the first statement cannot be retried and aborts with a StaleConfig /
// TransientTransactionError.
//
// TODO SERVER-37207: Change batch writes to retry only the failed writes in a batch, to allow
// retrying writes beyond the first overall statement. If that lands, a stale write sent to only new
// participant shards may become retryable and succeed, and this assertion should be updated.

// The two writes target different collections so the version-stale sibling move doesn't co-locate
// the statements' chunks: the first write targets foo on Shard1, and the second targets bar's
// [0, MaxKey) chunk, which we place on Shard0 (a distinct, not-yet-participant shard) with its
// sibling on Shard2.
assert.commandWorked(
    txnRouter.adminCommand({moveChunk: otherNs, find: {_id: -5}, to: st.shard2.shardName}),
);
expectChunks(st, otherNs, [1, 0, 1]);

res = runWithVersionStaleRouter(otherNs, 5, () => {
    advanceSessionPastMigrations();
    session.startTransaction();
    // The first write targets foo on Shard1 (a participant), not stale.
    assert.commandWorked(sessionDB.runCommand({insert: collName, documents: [{_id: 8}]}));
    // The stale write targets bar on Shard0 (a new participant). Because writes cannot be retried
    // past the first statement, it aborts the transaction with a StaleConfig error.
    return assert.commandFailedWithCode(
        sessionDB.runCommand({insert: otherCollName, documents: [{_id: 8}]}),
        ErrorCodes.StaleConfig,
    );
});
assert.eq(res.errorLabels, ["TransientTransactionError"]);

assertNoSuchTransactionOnAllShards(st, session.getSessionId(), session.getTxnNumber_forTesting());
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

//
// The final StaleConfig error should be returned if the router exhausts its retries.
//

// Place a chunk on Shard0 so the statement below targets it. Shard0 is forced to return StaleConfig
// via a failpoint (below) rather than by making it metadata-stale, so no router staleness is needed.
assert.commandWorked(
    txnRouter.adminCommand({moveChunk: ns, find: {_id: -5}, to: st.shard0.shardName}),
);
expectChunks(st, ns, [1, 1, 0]);

session.startTransaction();

// Target Shard1, to verify the transaction on it is aborted implicitly later.
assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: 5}}));

// Make metadata checks on Shard0 indefinitely return StaleConfig.
const fp = configureFailPoint(st.rs0.getPrimary(), "alwaysThrowStaleConfigInfo");

// Targets all shards. Shard0 always returns StaleConfig and never resolves, so mongos exhausts its
// retries and implicitly aborts the transaction.
res = assert.commandFailedWithCode(sessionDB.runCommand({find: collName}), ErrorCodes.StaleConfig);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

// Verify the shards that did not return an error were also aborted.
assertNoSuchTransactionOnAllShards(st, session.getSessionId(), session.getTxnNumber_forTesting());
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

fp.off();

disableStaleVersionAndSnapshotRetriesWithinTransactions(st);

st.stop();
