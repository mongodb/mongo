/**
 * Tests that session migration recognizes a retryable applyOps oplog entry tagged with
 * MultiOplogEntryType::kApplyOpsAppliedAtomically (SERVER-126372). A retryable bulkWrite whose
 * statements all target a single shard produces one applyOps oplog entry. With the dependent
 * isRetryableWriteApplyOps() predicate from SERVER-126368, the session catalog migration source
 * must extract the inner statements and migrate them so that a retry after moveChunk on the
 * recipient is correctly de-duplicated.
 *
 * @tags: [
 *   requires_fcv_80,
 *   uses_transactions,
 *   requires_persistence,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {testMoveChunkWithSession} from "jstests/sharding/move_chunk_with_session_helper.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 2},
    mongosOptions: {setParameter: {featureFlagBulkWriteCommand: true}},
});

const dbName = "test";
const collName = "atomic_retryable_apply_ops";
const ns = dbName + "." + collName;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

// A retryable bulkWrite with several statements targeting one nsInfo entry produces a single
// applyOps oplog entry on the donor. Under the SERVER-126368/126372 patch the entry carries
// MultiOplogEntryType::kApplyOpsAppliedAtomically; session migration must follow that tag and
// transfer the inner statement ids to the recipient.
const cmd = {
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 1, x: 10}},
        {insert: 0, document: {_id: 2, x: 11}},
        {insert: 0, document: {_id: 3, x: 12}},
    ],
    nsInfo: [{ns: ns}],
    txnNumber: NumberLong(0),
};

const checkRetryResult = function (result, retryResult) {
    assert.eq(result.ok, retryResult.ok);
    assert.docEq(result.cursor, retryResult.cursor);
    assert.eq(result.numErrors, retryResult.numErrors);
};

const checkDocuments = function (coll) {
    // Each statement must appear exactly once: session migration suppressed the re-execution
    // on the recipient because the atomic-tagged retryable applyOps was recognized.
    assert.eq(1, coll.countDocuments({_id: 1, x: 10}));
    assert.eq(1, coll.countDocuments({_id: 2, x: 11}));
    assert.eq(1, coll.countDocuments({_id: 3, x: 12}));
};

testMoveChunkWithSession(
    st,
    collName,
    cmd,
    function (coll) {},
    checkRetryResult,
    checkDocuments,
    true,
);

st.stop();
