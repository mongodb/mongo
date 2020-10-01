/**
 * Exercises transaction timing out at various stages of the two-phase commit.
 * TODO SERVER-51325: this test can use more scenarios.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

(function() {
'use strict';

load('jstests/sharding/libs/sharded_transactions_helpers.js');

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

// The test should not depend on a particular timeout, but shorter timeout makes it faster.
TestData.transactionLifetimeLimitSeconds = 5;

let lsid = {id: UUID()};
let txnNumber = 0;

(function() {
let st = new ShardingTest({
    shards: 3,
    rs0: {nodes: [{}]},
    causallyConsistent: true,
    other: {mongosOptions: {verbose: 3}}
});

let coordinatorReplSetTest = st.rs0;

let participant0 = st.shard0;
let participant1 = st.shard1;
let participant2 = st.shard2;

const runCommitThroughMongosInParallelShellExpectAbort = function() {
    const runCommitExpectCode = "assert.commandFailedWithCode(db.adminCommand({" +
        "commitTransaction: 1," +
        "lsid: " + tojson(lsid) + "," +
        "txnNumber: NumberLong(" + txnNumber + ")," +
        "stmtId: NumberInt(0)," +
        "autocommit: false," +
        "})," +
        "ErrorCodes.NoSuchTransaction);";
    return startParallelShell(runCommitExpectCode, st.s.port);
};

const setUp = function() {
    // Create a sharded collection with a chunk on each shard:
    // shard0: [-inf, 0)
    // shard1: [0, 10)
    // shard2: [10, +inf)
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: participant0.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));

    flushRoutersAndRefreshShardMetadata(st, {ns});

    // Start a new transaction by inserting a document onto each shard.
    assert.commandWorked(st.s.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: -5}, {_id: 5}, {_id: 15}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }));
};

const testCommitProtocol = function(failpointData) {
    jsTest.log("Testing commit protocol with failpointData: " + tojson(failpointData));

    txnNumber++;
    setUp();

    let coordPrimary = coordinatorReplSetTest.getPrimary();

    assert.commandWorked(coordPrimary.adminCommand({
        configureFailPoint: failpointData.failpoint,
        mode: {skip: (failpointData.skip ? failpointData.skip : 0)},
    }));

    // Run commitTransaction through a parallel shell.
    let awaitResult = runCommitThroughMongosInParallelShellExpectAbort();

    awaitResult();

    // Check that the transaction aborted as expected.
    jsTest.log("Verify that the transaction was aborted on all shards.");
    assert.eq(0, st.s.getDB(dbName).getCollection(collName).find().itcount());

    st.s.getDB(dbName).getCollection(collName).drop();
    clearRawMongoProgramOutput();
};

//
// Run through all the failpoints.
//

testCommitProtocol(getCoordinatorFailpoints().find((data) => data.failpoint ===
                                                       'hangBeforeWritingParticipantList'));

st.stop();
})();
})();
