/**
 * Confirms that retryable internal transactions initiated during resharding are correctly
 * logged in the recipient shard's oplog and are retryable on the recipient, post resharding.
 * We test receiving the first instance of the retryable write command during the cloning and
 * post-cloning stages of resharding as this is when writes to the oplog are triggered.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");
load('jstests/sharding/libs/sharded_transactions_helpers.js');
load("jstests/libs/discover_topology.js");
load('jstests/libs/auto_retry_transaction_in_sharding.js');

const reshardingTest = new ReshardingTest({numDonors: 1, numRecipients: 1, reshardInPlace: false});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const kDbName = "testDB";
const kCollName = "testColl";

const coll = reshardingTest.createShardedCollection({
    ns: kDbName + "." + kCollName,
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    primaryShardName: donorShardNames[0],
});

const mongos = coll.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const recipientConn = new Mongo(topology.shards[recipientShardNames[0]].primary);
const testDB = mongos.getDB(kDbName);

function runInsertCmdObj(conn, lsid, txnNumber, document, isTransaction) {
    const testDB = conn.getDB(kDbName);
    var cmdObj = {
        insert: kCollName,
        documents: [document],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
    };

    if (isTransaction) {
        cmdObj.startTransaction = true;
        cmdObj.autocommit = false;
    }

    var res = assert.commandWorked(testDB.runCommand(cmdObj));

    if (isTransaction) {
        const commitCmdObj = makeCommitTransactionCmdObj(lsid, NumberLong(txnNumber));
        assert.commandWorked(testDB.adminCommand(commitCmdObj));
    }

    assert.eq(1, res.n);
}

function createSessionOptsForTest() {
    const sessionID = UUID();
    const parentTxnNumber = NumberLong(5);
    return {
        parentLsid: {id: sessionID},
        parentTxnNumber: parentTxnNumber,
        childLsid: {id: sessionID, txnNumber: parentTxnNumber, txnUUID: UUID()},
        childTxnNumber: NumberLong(0)
    };
}

function runRetryableWrite(conn, sessionOpts, document) {
    runInsertCmdObj(conn, sessionOpts.parentLsid, sessionOpts.parentTxnNumber, document, false);
}

function runRetryableInternalTransaction(conn, sessionOpts, document) {
    try {
        runInsertCmdObj(conn, sessionOpts.childLsid, sessionOpts.childTxnNumber, document, true);
    } catch (e) {
        if (e.hasOwnProperty('errorLabels') &&
            e.errorLabels.includes('TransientTransactionError')) {
            ++sessionOpts.childTxnNumber;
            runRetryableInternalTransaction(conn, sessionOpts, document);
        } else {
            throw e;
        }
    }
}

// TODO (SERVER-63441): Uncomment testing variables.
// const sessionOpts0 = createSessionOptsForTest();
// const document0 = {
//     x: 0,
//     oldKey: 0,
//     newShardKey: 0
// };

const sessionOpts1 = createSessionOptsForTest();
const document1 = {
    x: 1,
    oldKey: 1,
    newShardKey: 1
};

const sessionOpts2 = createSessionOptsForTest();
const document2 = {
    x: 2,
    oldKey: 2,
    newShardKey: 2
};

// TODO (SERVER-63441): Enable commented tests.
// runRetryableInternalTransaction(mongos, sessionOpts0, document0);
// runRetryableWrite(mongos, sessionOpts0, document0);
// assert.eq(testDB.getCollection(kCollName).find(document0).itcount(), 1);

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newShardKey: 1},
        newChunks: [
            {min: {newShardKey: MinKey}, max: {newShardKey: MaxKey}, shard: recipientShardNames[0]}
        ],
    },
    () => {
        // Phase 1: During resharding, when coordinator is in cloning state.
        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: coll.getFullName()
            });

            return coordinatorDoc !== null && coordinatorDoc.state === "cloning";
        });

        runRetryableInternalTransaction(mongos, sessionOpts1, document1);
        runRetryableWrite(mongos, sessionOpts1, document1);
        assert.eq(testDB.getCollection(kCollName).find(document1).itcount(), 1);

        // Phase 2: During resharding, after collection cloning has finished.
        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: coll.getFullName()
            });
            return coordinatorDoc !== null && coordinatorDoc.state === "applying";
        });

        runRetryableInternalTransaction(mongos, sessionOpts1, document1);
        runRetryableWrite(mongos, sessionOpts1, document1);
        assert.eq(testDB.getCollection(kCollName).find(document1).itcount(), 1);

        runRetryableInternalTransaction(mongos, sessionOpts2, document2);
        runRetryableWrite(mongos, sessionOpts2, document2);
        assert.eq(testDB.getCollection(kCollName).find(document2).itcount(), 1);
    });

assert.commandWorked(mongos.adminCommand({flushRouterConfig: 1}));

// TODO (SERVER-63441): Enable commented test.
// Verify retryable writes performed before resharding are not retryable on the recipient shards.
// assert.commandFailedWithCode(runRetryableInternalTransaction(recipientConn, sessionOpts0,
// document0), ErrorCodes.IncompleteTransactionHistory);
// assert.commandFailedWithCode(runRetryableWrite(mongos, sessionOpts0, document0),
// ErrorCodes.IncompleteTransactionHistory);
// assert.eq(testDB.getCollection(kCollName).find(document0).itcount(), 1);

// We attempt retries of both transactions now that resharding is complete.
runRetryableInternalTransaction(recipientConn, sessionOpts1, document1);
runRetryableWrite(mongos, sessionOpts1, document1);
runRetryableInternalTransaction(recipientConn, sessionOpts2, document2);
runRetryableWrite(mongos, sessionOpts2, document2);
assert.eq(testDB.getCollection(kCollName).find(document1).itcount(), 1);
assert.eq(testDB.getCollection(kCollName).find(document2).itcount(), 1);

reshardingTest.teardown();
})();
