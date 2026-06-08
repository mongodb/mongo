// Test that a pipeline of the form [{$changeStream: {}}, {$match: ...}] can rewrite the
// 'wallTime' field and apply it to oplog-format documents in order to filter out results as early
// as possible. The 'wallTime' field maps directly to the oplog 'wall' field for all oplog entry
// types. For applyOps entries (transactions), the top-level 'wall' value equals the 'wallTime'
// of every change event produced from that entry, so the same simple rename applies there too.
// The wallTime predicate must NOT be pushed into the per-operation unwind filter, because
// individual operations inside o.applyOps do not carry their own 'wall' field.
// @tags: [
//   # Exclude this test from implicit transaction passthroughs, as these generate different
//   # oplog entries (one applyOps oplog entry instead of multiple individual oplog entries).
//   change_stream_does_not_expect_txns,
//   requires_fcv_90,
//   requires_pipeline_optimization,
//   requires_sharding,
//   uses_change_streams,
//   assumes_unsharded_collection,
//   assumes_read_preference_unchanged
// ]
import {
    createShardedCollection,
    verifyChangeStreamOnWholeCluster,
} from "jstests/libs/query/change_stream_rewrite_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "change_stream_match_pushdown_walltime_rewrite";
const collName = "coll1";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
});

// Create a sharded collection split at _id 2: _id < 2 goes to shard0, _id >= 2 to shard1.
const coll = createShardedCollection(st, "_id" /* shardKey */, dbName, collName, 2 /* splitAt */);

// Open a change stream and store the resume token. This resume token will be used to replay the
// stream after this point.
const resumeAfterToken = coll.watch([]).getResumeToken();

// A helper that opens a change stream on the whole cluster with the user-supplied match expression
// 'userMatchExpr' and validates that:
// 1. for each shard, the events are seen in that order as specified in 'expectedResult'
// 2. the filtering is been done at oplog level
// 3. the number of docs returned by the oplog cursor on each shard matches what we expect
//    as specified in 'expectedOplogRetDocsForEachShard'.
function verifyOnWholeCluster(userMatchExpr, expectedResult, expectedOplogRetDocsForEachShard) {
    verifyChangeStreamOnWholeCluster({
        st: st,
        changeStreamSpec: {resumeAfter: resumeAfterToken},
        userMatchExpr: userMatchExpr,
        expectedResult: expectedResult,
        expectedOplogNReturnedPerShard: Array.isArray(expectedOplogRetDocsForEachShard)
            ? expectedOplogRetDocsForEachShard
            : [expectedOplogRetDocsForEachShard, expectedOplogRetDocsForEachShard],
    });
}

// These operations create oplog events that the change stream will observe.
// _id: 1 lands on shard0, _id: 2 lands on shard1.
assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(coll.insert({_id: 2}));
assert.commandWorked(coll.update({_id: 1}, {$set: {foo: "bar"}}));
assert.commandWorked(coll.update({_id: 2}, {$set: {foo: "bar"}}));
assert.commandWorked(coll.deleteOne({_id: 1}));
assert.commandWorked(coll.deleteOne({_id: 2}));

// Verify that a $type predicate on 'wallTime' is correctly rewritten to apply to the oplog 'wall'
// field. The oplog 'wall' field is always a Date, so type 9 (date) matches every event, meaning
// the oplog cursor returns all docs and no events are filtered out.
verifyOnWholeCluster(
    {$match: {wallTime: {$type: [9]}}},
    {[collName]: {"insert": [1, 2], "update": [1, 2], "delete": [1, 2]}},
    3 /* 3 events per shard: insert, update, delete */,
);

// Verify that a $gt predicate with a date far in the past is correctly rewritten. All events were
// created after the Unix epoch (Jan 1 1970), so they all match and the oplog cursor returns all
// docs per shard.
verifyOnWholeCluster(
    {$match: {wallTime: {$gt: new Date(0)}}},
    {[collName]: {"insert": [1, 2], "update": [1, 2], "delete": [1, 2]}},
    3 /* 3 events per shard */,
);

// Verify that a $gt predicate with a date far in the future is correctly pushed down to the oplog.
// No events were created after year 2099, so the rewritten filter {wall: {$gt: new Date(...)}}
// eliminates all oplog entries at the cursor level, returning 0 docs per shard.
verifyOnWholeCluster(
    {$match: {wallTime: {$gt: new Date("2099-01-01")}}},
    {} /* no events expected */,
    0 /* the rewritten oplog filter matches nothing, so 0 docs returned per shard */,
);

// Verify that a $lt predicate with the Unix epoch is correctly pushed down to the oplog. No events
// were created before the Unix epoch, so the rewritten filter eliminates all oplog entries at the
// cursor level, returning 0 docs per shard.
verifyOnWholeCluster(
    {$match: {wallTime: {$lt: new Date(0)}}},
    {} /* no events expected */,
    0 /* the rewritten oplog filter matches nothing, so 0 docs returned per shard */,
);

// Verify that an $exists: true predicate on 'wallTime' is correctly rewritten. The 'wall' field
// always exists in oplog entries, so all events pass the filter and the oplog cursor returns all
// docs per shard.
verifyOnWholeCluster(
    {$match: {wallTime: {$exists: true}}},
    {[collName]: {"insert": [1, 2], "update": [1, 2], "delete": [1, 2]}},
    3 /* 3 events per shard */,
);

// Verify that an $exists: false predicate on 'wallTime' is correctly rewritten. The 'wall' field
// always exists in oplog entries, so no events pass the filter and the oplog cursor returns no docs
// on any shard.
verifyOnWholeCluster({$match: {wallTime: {$exists: false}}}, {}, 0 /* no events per shard */);

// Verify that an $expr predicate on 'wallTime' is correctly rewritten. The $expr uses $gt to
// compare wallTime against a far-future date, so no events match and 0 oplog docs are returned.
verifyOnWholeCluster(
    {$match: {$expr: {$gt: ["$wallTime", new Date("2099-01-01")]}}},
    {} /* no events expected */,
    0 /* the rewritten oplog filter matches nothing, so 0 docs returned per shard */,
);

//
// Test that wallTime predicates work correctly for applyOps (transaction) events.
//
// ApplyOps entries are passed through the oplog-level filter without wallTime filtering,
// because individual operations within o.applyOps do not have their own "wall" field.
// The wallTime predicate is applied at the pipeline level when events are unwound.
//

// Capture a resume token after the initial CRUD operations, so the transaction tests start fresh.
const resumeAfterCrudToken = coll.watch([]).getResumeToken();

// Insert a document via a transaction. _id: 3 goes to shard1.
{
    const session = st.s.startSession();
    session.startTransaction();
    assert.commandWorked(session.getDatabase(dbName)[collName].insert({_id: 3}));
    session.commitTransaction();
    session.endSession();
}

// Verify that a wallTime $gt predicate with a past date includes the transaction event.
// The transaction's wallTime (= the applyOps commit wall time) is after the Unix epoch, so it
// must match and appear in the change stream output.
{
    const cs = coll.watch([{$match: {wallTime: {$gt: new Date(0)}}}], {
        resumeAfter: resumeAfterCrudToken,
        maxAwaitTimeMS: 5000,
    });
    assert.soon(() => cs.hasNext(), "Expected a transaction insert event with wallTime > epoch");
    const event = cs.next();
    assert.eq(event.operationType, "insert", event);
    assert.eq(event.documentKey._id, 3, event);
    cs.close();
}

// Capture a resume token after the single document txn.
const resumeAfterTxnToken = coll.watch([]).getResumeToken();
const txnEventCount = 10;

// Insert multiple documents via a transaction. _id: All documents go to shard1.
{
    const session = st.s.startSession();
    session.startTransaction();
    for (let i = 0; i < txnEventCount; ++i) {
        assert.commandWorked(session.getDatabase(dbName)[collName].insert({_id: 10 + i}));
    }
    session.commitTransaction();
    session.endSession();
}

// Verify that a wallTime $gt predicate with a past date includes the transaction event.
// The transaction's wallTime (= the applyOps commit wall time) is after the Unix epoch, so it
// must match and appear in the change stream output.
{
    const cs = coll.watch([{$match: {wallTime: {$gt: new Date(0)}}}], {
        resumeAfter: resumeAfterTxnToken,
        maxAwaitTimeMS: 5000,
    });
    for (let i = 0; i < txnEventCount; ++i) {
        assert.soon(() => cs.hasNext(), "Expected a transaction insert event with wallTime > epoch");
        const event = cs.next();
        assert.eq(event.operationType, "insert", event);
        assert.eq(event.documentKey._id, 10 + i, event);
    }
    cs.close();
}

// Verify that a wallTime $gt predicate with a far-future date excludes the transaction event.
// The transaction's wallTime is not after year 2099, so no events should appear.
// The applyOps entry passes the oplog-level filter (command entries are always included), but
// the pipeline-level predicate filters out the resulting event.
{
    const cs = coll.watch([{$match: {wallTime: {$gt: new Date("2099-01-01")}}}], {
        resumeAfter: resumeAfterTxnToken,
        maxAwaitTimeMS: 1000,
    });
    // The transaction event has a recent wallTime (not > 2099), so it must not appear.
    assert(!cs.hasNext(), "Expected no events when wallTime > 2099 filter applied to transaction");
    cs.close();
}

st.stop();
