/**
 * SERVER-54019 reproducer: session migration from moveChunk can lead to higher 'n' and
 * 'nModified' for retryable updates by _id.
 *
 * Background
 *   When mongos broadcasts an `update` batch with `ordered: false` containing more than one
 *   update-one by _id (without a shard key), every shard that owns chunks for the collection
 *   receives the statement. Each shard records the statement in its config.transactions table
 *   under (lsid, txnNumber, stmtId). Only the shard owning the matching _id reports `n: 1`;
 *   the others report `n: 0`. mongos sums per-statement, producing the expected `{n: 1,
 *   nModified: 1}` on the initial run.
 *
 *   The bug: when moveChunk migrates a chunk that does NOT contain the target _id, the session
 *   information (config.transactions row for stmtId=0) is still copied from the donor to the
 *   recipient as part of session migration. On retry of the same (lsid, txnNumber), the
 *   recipient now answers "already-applied" and reports `{n: 1, nModified: 1}` -- but so does
 *   the original owning shard. mongos sums them and returns `{n: 2, nModified: 2}`, which
 *   contradicts the retryable-write idempotency contract.
 *
 * Reproducer shape mirrors the snippet attached to SERVER-54019 by Max Hirschhorn (2021-01-25)
 * and is structured to:
 *   1. PASS cleanly if the bug is fixed (returns {n: 1, nModified: 1} on retry).
 *   2. FAIL with an informative message if the bug reproduces ({n: 2, nModified: 2}) so the
 *      diagnostic shows up directly in resmoke output.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_fcv_60,
 * ]
 */
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, config: 1, shards: 2, rs: {nodes: 1}});

const db = st.s.getDB("test");
const coll = db.getCollection("retryable_update_by_id");

// Lay out four chunks across two shards. The target _id will live on shard0 (chunk
// {x: 0..10}); the second update predicate (_id: 10000) intentionally matches nothing,
// forcing mongos to broadcast the batch to both shards because the updates are by _id without
// the shard key.
CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
    {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: 10}, shard: st.shard0.shardName},
    {min: {x: 10}, max: {x: 20}, shard: st.shard1.shardName},
    {min: {x: 20}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

// Seed exactly one document. _id: 0 lives at x: 5, which sits in the {x: 0..10} chunk on shard0.
assert.commandWorked(coll.insert({_id: 0, x: 5, counter: 0}));

// Use a manually-managed session: retryWrites:false on the client so the driver does NOT add
// a retry layer; we drive (lsid, txnNumber) explicitly to make the bug visible. The session
// is opaque to the bug -- it is the *server-side* config.transactions copy via moveChunk that
// matters.
const session = st.s.startSession({causalConsistency: false, retryWrites: false});
const sessionDb = session.getDatabase(db.getName());
const sessionColl = sessionDb.getCollection(coll.getName());

// ordered:false + multiple update-ones by _id without the shard key is exactly the regime
// the Last Comment on SERVER-54019 calls out as still-broken after SPM-3190's partial fix.
// {q: {_id: 0}}      -- matches the seeded doc on shard0
// {q: {_id: 10000}}  -- matches no document anywhere; forces broadcast to all shards
const updateCmd = {
    updates: [
        {q: {_id: 0}, u: {$inc: {counter: 1}}},
        {q: {_id: 10000}, u: {$inc: {counter: 1}}},
    ],
    ordered: false,
    txnNumber: NumberLong(0),
};

jsTest.log("First execution of retryable update batch (pre-migration).");
const firstRes = assert.commandWorked(sessionColl.runCommand("update", updateCmd));
assert.eq(
    {n: firstRes.n, nModified: firstRes.nModified},
    {n: 1, nModified: 1},
    `Pre-migration result must be {n: 1, nModified: 1} but was ${tojson(firstRes)}`,
);
assert.eq(1, coll.findOne({_id: 0}).counter, "counter must be 1 after first update");

jsTest.log("Moving chunk owning _id: 0 from shard0 to shard1; session info migrates with it.");
assert.commandWorked(
    db.adminCommand({moveChunk: coll.getFullName(), find: {x: 5}, to: st.shard1.shardName, _waitForDelete: true}),
);

// Force the router to refresh its routing table so the retry is dispatched against the new
// placement. (A retry of a retryable write does not itself trigger a refresh.)
assert.commandWorked(st.s.adminCommand({flushRouterConfig: 1}));

jsTest.log("Retrying the same (lsid, txnNumber) batch after moveChunk.");
const secondRes = assert.commandWorked(sessionColl.runCommand("update", updateCmd));

// Documented (correct) behaviour: the retry must be recognised as a retry of the original
// statement and return the same response shape: {n: 1, nModified: 1}. The seeded document
// must NOT be re-incremented.
const observed = {n: secondRes.n, nModified: secondRes.nModified};
const expected = {n: 1, nModified: 1};

if (bsonWoCompare(observed, expected) !== 0) {
    // Bug reproduced. Emit a structured failure so resmoke surfaces the exact symptom.
    jsTest.log.error("SERVER-54019 reproduced: retry returned inflated counts after moveChunk", {
        observed: observed,
        expected: expected,
        rawResult: secondRes,
    });
    assert(
        false,
        `SERVER-54019 reproduced -- retry of (lsid, txnNumber=0) returned ${tojson(observed)}; ` +
            `expected ${tojson(expected)}. Session migration from moveChunk caused both shards to ` +
            `report {n: 1, nModified: 1} for stmtId=0, which mongos summed into ${tojson(observed)}.`,
    );
}

// Even if 'n' looked clean, the document itself must not have been double-incremented.
const counterAfterRetry = coll.findOne({_id: 0}).counter;
assert.eq(
    1,
    counterAfterRetry,
    `SERVER-54019 reproduced via document state: counter=${counterAfterRetry} after retry; ` +
        `expected counter=1 because the retry should have been treated as a no-op.`,
);

jsTest.log("Retry returned {n: 1, nModified: 1} and counter remained at 1; bug not reproduced on this build.");

session.endSession();
st.stop();
