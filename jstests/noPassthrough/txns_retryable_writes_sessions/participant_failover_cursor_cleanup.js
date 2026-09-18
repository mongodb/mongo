/**
 * Tests that a transaction participant failing over while a sharded $lookup cursor is being
 * advanced surfaces NoSuchTransaction to the client without terminating mongos, both when the
 * term mismatch is raised by the active getMore and when cursor cleanup is the first to observe
 * it.
 *
 * Scenario 1 (active path): the getMore raises the term mismatch and the pinned cursor is cleaned
 * up while that exception is still unwinding — cleanup must not re-raise it.
 *
 * Scenario 2 (cleanup path): the mismatch lands while the cursor is checked in, so cursor cleanup
 * is the first to observe it. The cleanup drain cannot throw, so it latches a deferred abort on
 * the router, which is raised at the end of the same killCursors command (the command's error
 * path runs the abort): the killCursors fails with the original NoSuchTransaction, and the
 * already-aborted transaction's later commitTransaction still fails without reaching its commit
 * path.
 *
 * @tags: [
 *   requires_getmore,
 *   requires_sharding,
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {checkLog} from "src/mongo/shell/check_log.js";

function controlledFailover(rst, label, foreignColl1, stepBack = false) {
    const oldPrimary = rst.getPrimary();
    const termBefore = oldPrimary.adminCommand({replSetGetStatus: 1}).term;
    const newPrimary = rst.getSecondaries()[0];
    jsTest.log.info(label, {oldPrimary: oldPrimary.host, newPrimary: newPrimary.host});
    rst.stepUp(newPrimary);
    rst.awaitNodesAgreeOnPrimary();
    assert.neq(oldPrimary.host, rst.getPrimary().host);
    if (stepBack) {
        rst.stepUp(oldPrimary);
        rst.awaitNodesAgreeOnPrimary();
        assert.eq(oldPrimary.host, rst.getPrimary().host);
    }
    const termAfter = rst.getPrimary().adminCommand({replSetGetStatus: 1}).term;
    assert.gt(termAfter, termBefore, "the failover must advance the replica set's term");
    // Settle mongos' routing state on the failed-over set (the find routes to foreignColl1's
    // chunk): routing must be refreshed outside the transaction, whose reads cannot refresh
    // stale routing while holding locks.
    foreignColl1.find().toArray();
    return termBefore;
}

const dbName = "test";
const localColl1 = "local_coll";
const foreignColl1 = "foreign_coll";
const foreignNs1 = dbName + "." + foreignColl1;

const st = new ShardingTest({
    // Scenario 2 needs a third shard that is recruited by the $lookup sub-routers without being
    // targeted by the transaction's cursor. Two nodes per shard suffice: each scenario only ever
    // steps up the one secondary. Keep the fixture small — slow variants (TSAN) have starved the
    // fixture's own setup (balancerStop's 60s cap) on larger ones.
    shards: 3,
    rs: {nodes: 2, setParameter: {transactionLifetimeLimitSeconds: 24 * 60 * 60}},
});

const mongosDB = st.s.getDB(dbName);

// Raise the mongos QUERY and TRANSACTION log components so the ARM markers and the
// implicit-abort marker (id 22897, debug level 3) are observable.
assert.commandWorked(
    st.s.getDB("admin").runCommand({
        setParameter: 1,
        logComponentVerbosity: {query: {verbosity: 3}, transaction: {verbosity: 3}},
    }),
);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs1, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: foreignNs1, middle: {_id: 2}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: foreignNs1, find: {_id: 2}, to: st.shard1.shardName}),
);
// Document layout once the docs below are inserted:
// - shard0:
//     - foreignColl1 chunk [MinKey, 2) — foreign docs {_id: 0, 1}.
//     - localColl1, unsharded — local docs {_id: 0, 1, 2}.
// - shard1:
//     - foreignColl1 chunk [2, MaxKey) — foreign docs {_id: 2, 3}.
st.refreshCatalogCacheForNs(st.s, foreignNs1);
if (!FeatureFlagUtil.isPresentAndEnabled(st.rs0.getPrimary(), "AuthoritativeShardsCRUD")) {
    assert.commandWorked(
        st.rs0.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: foreignNs1}),
    );
    assert.commandWorked(
        st.rs1.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: foreignNs1}),
    );
}

// The local docs, chosen so the sorted first batch (batchSize 1) contacts only the lower foreign
// chunk on shard0, and the getMore is what introduces shard1 after the controlled failover:
//   {_id: 0, key: 0} — key 0 → joins foreign _id:0 → foreign chunk [MinKey, 2) on shard0
//   {_id: 1, key: 0} — key 0 again → shard0
//   {_id: 2, key: 2} — key 2 → joins foreign _id:2 → foreign chunk [2, MaxKey) on shard1
const localKeys = [0, 0, 2];
const localDocs = localKeys.map((key, i) => ({_id: i, key}));
const foreignDocs = [];
for (let i = 0; i < 4; ++i) {
    foreignDocs.push({_id: i, value: "joined"});
}
assert.commandWorked(mongosDB[localColl1].insert(localDocs, {writeConcern: {w: "majority"}}));
assert.commandWorked(mongosDB[foreignColl1].insert(foreignDocs, {writeConcern: {w: "majority"}}));

// Warm the shard0 sub-router's catalog cache before entering the transaction. Snapshot reads in a
// transaction cannot refresh stale routing information while locks are held.
mongosDB[localColl1]
    .aggregate([
        {$lookup: {from: foreignColl1, localField: "key", foreignField: "_id", as: "joined"}},
    ])
    .toArray();

const lsid = {id: UUID()};
const txnNumber = NumberLong(1);

// Make both shards explicit outer-router transaction participants. The shard0 sub-router does not
// contact shard1 until the controlled getMore below.
assert.commandWorked(
    mongosDB.runCommand({
        insert: localColl1,
        documents: [{_id: "txn_marker"}],
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);
assert.commandWorked(
    mongosDB.runCommand({
        insert: foreignColl1,
        documents: [{_id: "txn_foreign_marker", value: "transactional"}],
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);

const lookupPipeline = [
    {$sort: {_id: 1}},
    {$lookup: {from: foreignColl1, localField: "key", foreignField: "_id", as: "joined"}},
];
const aggregateRes = assert.commandWorked(
    mongosDB.runCommand({
        aggregate: localColl1,
        pipeline: lookupPipeline,
        // Fetch only the first local document. It looks up a key on shard0; the remaining local
        // documents keep the cursor open and cause the getMore to contact shard1.
        cursor: {batchSize: 1},
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(2),
        autocommit: false,
    }),
);
assert.eq(1, aggregateRes.cursor.firstBatch.length, "expected one first-batch doc", {
    aggregateRes,
});
assert.neq(NumberLong(0), aggregateRes.cursor.id, "expected an open cursor", {aggregateRes});

// Pause the shard0-side getMore after it has pinned its cursor but before it executes the next
// batch. This puts the failover between the first response (old term) and the next response (new
// term) without relying on timing races. Restrict the failpoint to the user cursor so background
// getMores cannot consume the synchronization point.
const shard0GetMoreFp = configureFailPoint(
    st.rs0.getPrimary(),
    "waitWithPinnedCursorDuringGetMoreBatch",
    {nss: dbName + "." + localColl1, shouldCheckForInterrupt: true},
);

const getMoreThread = new Thread(
    function runGetMore(host, dbName, collName, cursorIdString, lsidString) {
        const conn = new Mongo(host);
        const sessionId = eval("(" + lsidString + ")");
        try {
            return {
                response: conn.getDB(dbName).runCommand({
                    getMore: eval("(" + cursorIdString + ")"),
                    collection: collName,
                    batchSize: 2,
                    lsid: sessionId,
                    txnNumber: NumberLong(1),
                    autocommit: false,
                }),
            };
        } catch (e) {
            // Preserve transport failures (including a process abort) for the main shell to
            // report instead of hiding them in the worker thread.
            return {exception: e.toString()};
        }
    },
    st.s.host,
    dbName,
    localColl1,
    tojson(aggregateRes.cursor.id),
    tojson(lsid),
);
getMoreThread.start();
shard0GetMoreFp.wait();

// The recruited participant's term before the failover — mongos recorded it when the marker
// insert made it a participant, and the failing getMore's error must name it as the previously
// observed term.
const termAtFirstContact = controlledFailover(
    st.rs1,
    "Stepping down the lookup participant",
    mongosDB[foreignColl1],
);

shard0GetMoreFp.off();
getMoreThread.join();
const getMoreData = getMoreThread.returnData();
assert(!getMoreData.exception, "getMore transport failed; a server likely crashed", {getMoreData});
assert.commandFailedWithCode(
    getMoreData.response,
    ErrorCodes.NoSuchTransaction,
    "the participant term change should surface as NoSuchTransaction instead of aborting the " +
        "server",
);
assert(
    getMoreData.response.errmsg &&
        getMoreData.response.errmsg.includes("changed primaries during the transaction"),
    "expected the participant-term mismatch",
    {response: getMoreData.response},
);
assert(
    getMoreData.response.errmsg.includes("previously observed term " + termAtFirstContact),
    "the recorded term must be the one observed at first contact",
    {response: getMoreData.response},
);
// ARM-path marker: the DEBUG log (id 13412900) is emitted only by the AsyncResultsMerger
// participant drain when it catches a metadata failure — the getMore announcement is drained
// from the queued-response path (nextReady/detach), not the sender's synchronous path.
checkLog.containsJson(st.s, 13412900);

// Scenario 2: the mismatch is first observed by cursor cleanup, not by a live command.
//   (1) aggregate with batchSize 0 — no pipeline runs, so nobody has recorded shard2's term yet.
//   (2) getMore — shard0's reply announces shard2's current term; the active drain records it
//       (first observation, no raise). shard1's getMore sits at the failpoint, so the batch is
//       filled from shard0 alone and the cursor is checked in with shard1's request outstanding.
//   (3) rs2 steps up (and back) — shard2's term advances, same host primary again.
//   (4) shard1's getMore resumes, re-reads shard2 at the new term, and its reply announcing the
//       new term is buffered on mongos while the cursor is checked in, so nothing drains it.
//   (5) killCursors — the cleanup drain validates the new term against the recorded one, catches
//       the raise, and latches a deferred abort (log 13412902); the latch is raised at the end of
//       the same command, whose error path aborts, so the killCursors fails with the original
//       NoSuchTransaction.
//   (6) commitTransaction — the transaction is already aborted, so the commit fails without ever
//       reaching its commit path.
jsTest.log.info("Scenario 2: cleanup-observed term mismatch latches a deferred abort");

// Topology for the steps above: mongos must hold one remote cursor per shard so the getMore can
// return with shard1's request outstanding, and the merge half must be empty — no $sort, else
// the sorted merge waits for the paused shard. $lookup only stays on the shards half when the
// foreign collection is sharded (document_source_lookup.cpp, foreignShardedLookupAllowed());
// otherwise mongos keeps a single merge cursor on a designated shard and the getMore deadlocks
// against the failpoint. So the foreign collection is sharded but left as one chunk on shard2:
// on shard0 or shard1 the sub-routers would join it locally and shard2 would never be recruited.
const dbName2 = "test2";
const localColl2 = "local_coll2";
const foreignColl2 = "foreign_coll2";
const localNs2 = dbName2 + "." + localColl2;
const foreignNs2 = dbName2 + "." + foreignColl2;
const mongosDB2 = st.s.getDB(dbName2);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName2, primaryShard: st.shard2.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: localNs2, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: localNs2, middle: {_id: 10}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: localNs2, find: {_id: 0}, to: st.shard0.shardName}),
);
assert.commandWorked(
    st.s.adminCommand({moveChunk: localNs2, find: {_id: 10}, to: st.shard1.shardName}),
);
// Document layout once the docs below are inserted:
// - shard0: localColl2 chunk [MinKey, 10) — local docs {_id: 0..9}.
// - shard1: localColl2 chunk [10, MaxKey) — local docs {_id: 10..19}.
// - shard2: foreignColl2's single chunk — all foreign docs {_id: 0..3}.
// Each shard runs $lookup over its own local chunk as a sub-router.
// Sharded, but deliberately never split or moved: its single chunk stays on the db primary
// (shard2), so both lookup sub-routers must recruit shard2.
assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs2, key: {_id: 1}}));
st.refreshCatalogCacheForNs(st.s, localNs2);
st.refreshCatalogCacheForNs(st.s, foreignNs2);
if (!FeatureFlagUtil.isPresentAndEnabled(st.rs0.getPrimary(), "AuthoritativeShardsCRUD")) {
    assert.commandWorked(
        st.rs0.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: foreignNs2}),
    );
    assert.commandWorked(
        st.rs1.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: foreignNs2}),
    );
}

const localDocs2 = [];
for (let i = 0; i < 20; ++i) {
    localDocs2.push({_id: i, key: i % 16});
}
assert.commandWorked(mongosDB2[localColl2].insert(localDocs2, {writeConcern: {w: "majority"}}));
assert.commandWorked(mongosDB2[foreignColl2].insert(foreignDocs, {writeConcern: {w: "majority"}}));

const lookupPipeline2 = [
    {$lookup: {from: foreignColl2, localField: "key", foreignField: "_id", as: "joined"}},
];

// Clear the log so the plan probe below reflects only this pipeline: earlier aggregates
// (scenario 1 setup) may have already emitted 22835 for their own plans.
assert.commandWorked(st.s.adminCommand({clearLog: "global"}));

// Warm the mongos and shard routing caches so the transactional aggregate does not need to
// refresh stale routing while holding locks.
mongosDB2[localColl2].aggregate(lookupPipeline2).toArray();

// The cleanup-latch scenarios need mongos to merge the shard cursors itself: the getMore must be
// able to return while a sibling request is still outstanding. Log id 22835 ("Dispatching merge
// pipeline to designated shard") is emitted only by dispatchMergingPipeline(), i.e. only when the
// merge tier was handed to a designated shard, which is what suites that enable the join optimizer
// or SBE $lookup pushdown produce. Detect the plan once from the warm-up aggregate and skip those
// scenarios there; scenario 1 does not depend on the merge location and still runs.
const mergeOnDesignatedShard = checkLog.checkContainsOnceJson(st.s, 22835);

// Drives the cleanup-latch scenario and returns the killCursors response (which must carry the
// latched error) and the recruited shard's pre-failover term, which that error must name:
// (1) Establish the cursor with an empty first batch: the shards execute nothing, so neither
//     sub-router has contacted shard2 and the first observation of its term is mongos' own (from
//     shard0's reply in step 2). A sub-router that recorded it first would raise the mismatch
//     itself, and the ARM does not buffer the error response that would follow.
// (2) Pause shard1's getMore after it pins its cursor but before it produces the batch; the
//     unsorted merge answers from shard0 alone and checks the cursor back in with shard1's request
//     outstanding.
// (3) Fail over the recruited participant.
// (4) Release shard1's getMore and wait for its response — carrying the mismatched term — to land
//     in mongos' ARM queue.
// (5) Kill the cursor inside the transaction: the cleanup drain observes the queued mismatch, must
//     not throw mid-cleanup, and latches a deferred abort on the router — which executes at the end
//     of this command, failing the killCursors with the original NoSuchTransaction.
function driveCleanupLatchScenario(lsid, txnNumber) {
    assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
    const aggregateRes = assert.commandWorked(
        mongosDB2.runCommand({
            aggregate: localColl2,
            pipeline: lookupPipeline2,
            cursor: {batchSize: 0},
            lsid: lsid,
            txnNumber: txnNumber,
            stmtId: NumberInt(0),
            startTransaction: true,
            autocommit: false,
        }),
    );
    assert.neq(NumberLong(0), aggregateRes.cursor.id, "expected an open cursor", {aggregateRes});

    // Pin the premise: mongos must be merging the shard cursors itself. Log id 22835 is emitted
    // only by dispatchMergingPipeline(), i.e. only when the merge tier was handed to a designated
    // shard — in which case mongos would hold a single cursor and the getMore below could not
    // return while a shard is paused. Fail fast and loudly here rather than deadlocking.
    assert(
        !checkLog.checkContainsOnceJson(st.s, 22835),
        "mongos dispatched the merge pipeline to a shard, so it holds one cursor instead of one " +
            "per shard; this scenario requires a router-merged pipeline (is the foreign " +
            "collection still sharded?)",
    );

    const shard1GetMoreFp = configureFailPoint(
        st.rs1.getPrimary(),
        "waitWithPinnedCursorDuringGetMoreBatch",
        {nss: dbName2 + "." + localColl2, shouldCheckForInterrupt: true},
    );
    const getMoreRes = assert.commandWorked(
        mongosDB2.runCommand({
            getMore: aggregateRes.cursor.id,
            collection: localColl2,
            batchSize: 2,
            lsid: lsid,
            txnNumber: txnNumber,
            autocommit: false,
        }),
    );
    assert.eq(2, getMoreRes.cursor.nextBatch.length, "getMore should return shard0's docs", {
        getMoreRes,
    });
    shard1GetMoreFp.wait();

    // Step up the secondary and then back, so the shard2 host shard0 already talked to is primary
    // again with a bumped term. That keeps a possibly-stale replica set monitor view on shard1
    // correct — a one-way step up would have shard1's sub-router target the old primary and fail
    // with a not-primary error instead of reporting the new term. The settle read inside
    // controlledFailover deliberately targets the foreign collection: reading local_coll2 would be
    // caught by its still-armed getMore failpoint and deadlock against fp.off().
    const termBefore = controlledFailover(
        st.rs2,
        "Stepping down the recruited participant",
        mongosDB2[foreignColl2],
        /*stepBack*/ true,
    );

    // Drop everything logged so far so the 13412904 wait below only observes shard1's reply.
    assert.commandWorked(st.s.adminCommand({clearLog: "global"}));
    shard1GetMoreFp.off();

    // Wait for shard1's released response — carrying the mismatched term — to land in mongos' ARM
    // queue: the wait is on the DEBUG log emitted right after the _remoteResponses.push (the
    // buffering happens in an executor callback, invisible to $currentOp), filtered to shard1's
    // reply — the log's shardId attr is the responding shard, so shard0's earlier reply (step 2)
    // logged a different shardId. The push implies the shard-side getMore finished, so no
    // shard-side wait is needed.
    checkLog.containsJson(st.s, 13412904, {shardId: st.shard1.shardName});

    // Killing the cursor inside the transaction runs the cleanup drain over the queued mismatch.
    // The drain cannot throw, so it latches a deferred abort on the router — raised at the end of
    // this same command, where the command's error path runs the abort: the killCursors fails with
    // the latched NoSuchTransaction.
    const killRes = mongosDB2.runCommand({
        killCursors: localColl2,
        cursors: [aggregateRes.cursor.id],
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
    });
    // The latch marker (id 13412902) is logged only when a cleanup drain records a deferred abort.
    checkLog.containsJson(st.s, 13412902);

    return {killRes, termBefore};
}

// The latched error must surface with its original code and message on the command whose cleanup
// observed the failure.
function assertLatchedNoSuchTransaction(res, termBeforeFailover) {
    assert.commandFailedWithCode(
        res,
        ErrorCodes.NoSuchTransaction,
        "the latched deferred abort must surface on the observing command",
    );
    assert(
        res.errmsg && res.errmsg.includes("changed primaries during the transaction"),
        "expected the participant-term mismatch",
        {res},
    );
    assert(
        res.errmsg.includes("previously observed term " + termBeforeFailover),
        "the recorded term must be the one observed before the failover",
        {res},
    );
}

if (mergeOnDesignatedShard) {
    jsTest.log.info(
        "Skipping the cleanup-latch scenarios: mongos dispatched the merge pipeline to a shard, " +
            "so it holds one cursor instead of one per shard; they require a router-merged pipeline",
    );
} else {
    const lsid2 = {id: UUID()};
    const txnNumber2 = NumberLong(1);
    const {killRes: killRes2, termBefore: term2BeforeFailover} = driveCleanupLatchScenario(
        lsid2,
        txnNumber2,
    );

    // The deferred abort latched by the cleanup drain is raised at the end of the killCursors
    // command itself: the command's error path runs the implicit abort, and killCursors fails with
    // the original NoSuchTransaction.
    assertLatchedNoSuchTransaction(killRes2, term2BeforeFailover);

    // The transaction is already aborted, so a further statement fails with a plain
    // NoSuchTransaction, without the latched term-mismatch message.
    const findRes2 = mongosDB2.runCommand({
        find: localColl2,
        filter: {},
        lsid: lsid2,
        txnNumber: txnNumber2,
        autocommit: false,
    });
    assert.commandFailedWithCode(findRes2, ErrorCodes.NoSuchTransaction);

    // The commit still fails — as a commit retry on an already-aborted transaction, without the
    // latched message.
    const commitRes2 = st.s.adminCommand({
        commitTransaction: 1,
        lsid: lsid2,
        txnNumber: txnNumber2,
        autocommit: false,
    });
    assert.commandFailedWithCode(
        commitRes2,
        ErrorCodes.NoSuchTransaction,
        "a doomed transaction must never commit",
    );

    // An explicit abortTransaction on the already-aborted transaction also fails with a plain
    // NoSuchTransaction (the participants' answer to the second abort).
    const abortRes2 = st.s.adminCommand({
        abortTransaction: 1,
        lsid: lsid2,
        txnNumber: txnNumber2,
        autocommit: false,
    });
    assert.commandFailedWithCode(abortRes2, ErrorCodes.NoSuchTransaction);

    // mongos stayed alive and the implicit abort covered all three participants (the two targeted
    // shards plus the recruited one).
    assert.commandWorked(mongosDB.runCommand({ping: 1}));
    checkLog.containsJson(st.s, 22897, {numParticipantShards: 3}, 10 * 1000);
}

st.stop();
