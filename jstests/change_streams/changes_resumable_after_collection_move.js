/**
 * SERVER-86751: Companion coverage test for the
 * change_streams_mongos_passthrough_with_balancer suite.
 *
 * Verifies that a change stream opened against a mongos router on an
 * unsharded collection:
 *   1. continues to deliver inserts whose oplog entries are produced
 *      *after* the collection has been relocated to a different shard via
 *      `moveCollection`, and
 *   2. can be resumed from a token captured *before* the move and replay
 *      every post-move write.
 *
 * The passthrough suite supplies the background `moveCollection` activity
 * via the ContinuousMoveCollection hook for the rest of jstests/change_streams/**;
 * this companion exercises the same surface explicitly so the suite has at
 * least one test whose failure mode is unambiguously "stream lost an event
 * across a routing-table change".
 *
 *  @tags: [
 *    requires_fcv_80,
 *    requires_sharding,
 *    uses_change_streams,
 *    change_stream_does_not_expect_txns,
 *    assumes_read_preference_unchanged,
 *  ]
 */
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "test";
const ns = {db: dbName, coll: collName};

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
    other: {
        // Long resharding critical-section budget so `moveCollection` (which is
        // implemented via resharding under the hood for unsharded collections)
        // doesn't time out the test on slow variants.
        configOptions:
            {setParameter: {reshardingCriticalSectionTimeoutMillis: 24 * 60 * 60 * 1000}},
    },
});

const mongos = st.s;
const db = mongos.getDB(dbName);
const coll = db[collName];

// Place the database (and therefore the unsharded `coll`) on shard0.
assert.commandWorked(
    mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(db.runCommand({create: collName}));

// Pre-move insert so we have a known oplog event to resume from.
assert.commandWorked(coll.insert({_id: 0, phase: "pre-move"}));

const cst = new ChangeStreamTest(db);
const pipeline = [{$changeStream: {}}];
const cursor = cst.startWatchingChanges({pipeline, collection: collName});

// Capture a resume token whose clusterTime predates the move. We grab it from
// the oplog event for the pre-move insert that the stream is about to yield.
const preMoveEvent = cst.assertNextChangesEqual({
    cursor: cursor,
    expectedChanges: [{
        operationType: "insert",
        ns: ns,
        fullDocument: {_id: 0, phase: "pre-move"},
        documentKey: {_id: 0},
    }],
})[0];
const resumeToken = preMoveEvent._id;
assert(resumeToken, "expected the pre-move insert event to carry a resume token");

// Move the unsharded collection to shard1. This is the topology mutation the
// passthrough suite stresses continuously.
assert.commandWorked(mongos.adminCommand({
    moveCollection: coll.getFullName(),
    toShard: st.shard1.shardName,
}));

// Post-move inserts. These oplog entries are produced on the new owning shard
// (shard1) and must still be routed back through mongos to the stream that
// was opened against shard0's view of the collection.
for (let i = 1; i <= 5; i++) {
    assert.commandWorked(coll.insert({_id: i, phase: "post-move"}));
}

// 1) The originally-open cursor must deliver every post-move insert in order
// without losing an event across the routing-table flip.
cst.assertNextChangesEqual({
    cursor: cursor,
    expectedChanges: [
        {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: 1, phase: "post-move"},
            documentKey: {_id: 1},
        },
        {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: 2, phase: "post-move"},
            documentKey: {_id: 2},
        },
        {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: 3, phase: "post-move"},
            documentKey: {_id: 3},
        },
        {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: 4, phase: "post-move"},
            documentKey: {_id: 4},
        },
        {
            operationType: "insert",
            ns: ns,
            fullDocument: {_id: 5, phase: "post-move"},
            documentKey: {_id: 5},
        },
    ],
});

// 2) A fresh cursor resumed from the pre-move token must replay every
// post-move insert. This is the resumability invariant that the suite is
// designed to guard.
const resumedCursor = cst.startWatchingChanges({
    pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
    collection: collName,
});

cst.assertNextChangesEqual({
    cursor: resumedCursor,
    expectedChanges: [
        {operationType: "insert", ns: ns, fullDocument: {_id: 1, phase: "post-move"}},
        {operationType: "insert", ns: ns, fullDocument: {_id: 2, phase: "post-move"}},
        {operationType: "insert", ns: ns, fullDocument: {_id: 3, phase: "post-move"}},
        {operationType: "insert", ns: ns, fullDocument: {_id: 4, phase: "post-move"}},
        {operationType: "insert", ns: ns, fullDocument: {_id: 5, phase: "post-move"}},
    ],
});

st.stop();
