/**
 * The slim oplog is a view on the oplog to support efficient lookup queries for resharding.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_replication,
 *   sbe_incompatible,
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/retryable_writes_util.js");

if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
    jsTestLog("Retryable writes are not supported, skipping test");
    return;
}

load("jstests/core/txns/libs/prepare_helpers.js");

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const rsConn = new Mongo(rst.getURL());
const oplog = rst.getPrimary().getDB("local")["oplog.rs"];
const slimOplog = rst.getPrimary().getDB("local")["system.resharding.slimOplogForGraphLookup"];
const session = rsConn.startSession({retryWrites: true});
const collection = session.getDatabase("test")["collection"];

{
    // Assert an oplog entry representing a retryable write projects a `prevOpTime.ts` of the null
    // timestamp.
    assert.commandWorked(collection.insert({_id: "slim"}));

    const oplogEntry = oplog.find({"o._id": "slim"}).next();
    jsTestLog({"slim entry": oplogEntry, "slim": slimOplog.exists()});
    assert(oplogEntry.hasOwnProperty("txnNumber"));
    assert(oplogEntry.hasOwnProperty("stmtId"));
    assert.eq(Timestamp(0, 0), slimOplog.find({ts: oplogEntry["ts"]}).next()["prevOpTime"]["ts"]);
}

{
    // Assert an oplog entry that commits a prepared transaction projects a `prevOpTime.ts` equal to
    // the timestamp of the preparing oplog entry.
    session.startTransaction();
    assert.commandWorked(collection.insert({_id: "slim_1"}));
    assert.commandWorked(collection.insert({_id: "slim_2"}));
    let prepTs = PrepareHelpers.prepareTransaction(session);
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepTs));

    const commitEntry = oplog.find({"prevOpTime.ts": prepTs}).next();
    assert.eq(prepTs, commitEntry["prevOpTime"]["ts"]);
    jsTestLog({
        PrepEntry: oplog.find({ts: prepTs}).next(),
        CommitEntry: commitEntry,
        SlimEntry: slimOplog.find({ts: commitEntry["ts"]}).next()
    });

    // Perform a $graphLookup connecting non-trivial slim oplog entries to the associated applyOps
    // operations. Assert the history is as expected.
    let oplogDocsWithHistory = oplog.aggregate([
        {$match: {ts: commitEntry["ts"]}},
        {$graphLookup: {from: "system.resharding.slimOplogForGraphLookup",
                        startWith: "$ts",
                        connectFromField: "prevOpTime.ts",
                        connectToField: "ts",
                        depthField: "depthForResharding",
                        as: "history"}},
         // For the purposes of this test, unwind the history to give a predictable order of items
         // that graphLookup accumulates.
        {$unwind: "$history"},
        {$sort: {"history.ts": 1}}
    ]).toArray();

    jsTestLog({"Unwound history": oplogDocsWithHistory});
    assert.eq(2, oplogDocsWithHistory.length);
    assert.eq(1, oplogDocsWithHistory[0]["history"]["depthForResharding"]);
    assert.eq(Timestamp(0, 0), oplogDocsWithHistory[0]["history"]["prevOpTime"]["ts"]);
    assert.eq(2, oplogDocsWithHistory[0]["history"]["o"]["applyOps"].length);

    assert.eq(0, oplogDocsWithHistory[1]["history"].depthForResharding);
    assert.eq(prepTs, oplogDocsWithHistory[1]["history"]["prevOpTime"]["ts"]);
    assert.eq({}, oplogDocsWithHistory[1]["history"]["o"]);
}

rst.stopSet();
})();
