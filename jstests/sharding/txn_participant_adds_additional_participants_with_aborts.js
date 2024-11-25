/*
 * Tests that additional participants are handled correctly when a transaction is aborted.
 * @tags: [
 *   requires_fcv_80,
 *    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 *   uses_multi_shard_transaction,
 *   uses_transactions]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const customTransactionLifetimeLimitSeconds = 10;

/**
 * Checks that aborted transaction count was incremented for mongos and each participating shard.
 */
const verifyFinalAbortedTransactionMetrics = function(initialMongosTxnMetrics,
                                                      initialMongodTxnMetrics,
                                                      finalMongosTxnMetrics,
                                                      finalMongodTxnMetrics,
                                                      allParticipants,
                                                      expectedParticipants) {
    assert.gte(finalMongosTxnMetrics.totalAborted, initialMongosTxnMetrics.totalAborted + 1);
    finalMongodTxnMetrics.forEach((txnMetrics, index) => {
        if (expectedParticipants.includes(allParticipants[index])) {
            assert.gte(txnMetrics.totalAborted, initialMongodTxnMetrics[index].totalAborted + 1);
        } else {
            assert.gte(txnMetrics.totalAborted, initialMongodTxnMetrics[index].totalAborted);
        }
    });
};

const dbName = "test";
const localColl = "foo";
const foreignColl = "bar";
const localNs = dbName + "." + localColl;
const foreignNs = dbName + "." + foreignColl;

/**
 * Sets up ShardingTest fixture.
 */
function setupShardingTest() {
    let st = new ShardingTest({shards: 3});

    let setServerParams = (conn) => {
        assert.commandWorked(conn.getDB('admin').runCommand({
            setParameter: 1,
            transactionLifetimeLimitSeconds: customTransactionLifetimeLimitSeconds,
        }));
    };

    st.rs0.nodes.forEach(setServerParams);
    st.rs1.nodes.forEach(setServerParams);
    st.rs2.nodes.forEach(setServerParams);

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

    return st;
}

/**
 * Sets up collections for the first two test cases.
 */
function setupCollections(st) {
    // Create a sharded collection, "foo", with the following chunk:
    // shard0: _id: [-inf, +inf)
    assert.commandWorked(st.s.adminCommand({shardCollection: localNs, key: {_id: 1}}));

    // Create a sharded collection, "bar", with the following chunks:
    // shard1: x: [-inf, 0)
    // shard2: x: [0, +inf)
    assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: foreignNs, middle: {x: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: foreignNs, find: {x: -10}, to: st.shard1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: foreignNs, find: {x: 0}, to: st.shard2.shardName}));

    // These forced refreshes are not strictly necessary; they just prevent extra TXN log lines
    // from the shards starting, aborting, and restarting the transaction due to needing to
    // refresh after the transaction has started.
    [st.shard0, st.shard1, st.shard2].forEach(shard => {
        assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: localNs}));
    });
    st.refreshCatalogCacheForNs(st.s, localNs);

    [st.shard0, st.shard1, st.shard2].forEach(shard => {
        assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));
    });
    st.refreshCatalogCacheForNs(st.s, foreignNs);

    assert.commandWorked(st.s.getDB(dbName).foo.insert([{_id: -5}]));        // will live on shard0
    assert.commandWorked(st.s.getDB(dbName).bar.insert([{_id: 1, x: -5}]));  // will live on shard1
}

/**
 * Drops collections from previous test.
 */
function dropCollections(st) {
    assert.commandWorked(
        st.s.getDB(dbName).runCommand({drop: localColl, writeConcern: {w: "majority"}}));
    assert.commandWorked(
        st.s.getDB(dbName).runCommand({drop: foreignColl, writeConcern: {w: "majority"}}));
}

const st = setupShardingTest();
const allParticipants = [st.shard0, st.shard1, st.shard2];

/* Test case 1 */
{
    jsTest.log("Testing that an additional participant is included in the abort protocol");
    dropCollections(st);
    setupCollections(st);

    const expectedParticipants = [st.shard0, st.shard1];
    const shardsWithForeignColl = [st.shard1];

    const session = st.s.startSession();
    const txnNum = 1;
    const sessionId = session.getSessionId();

    const initialMongosTxnMetrics =
        assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
    const initialMongodTxnMetrics = allParticipants.map((shard) => {
        return assert.commandWorked(shard.adminCommand({serverStatus: 1})).transactions;
    });

    // Set a failpoint on each of the shards we expect the participant shard to target that will
    // force them to hang while executing the agg request. This allows us to check the number of
    // participants contacted by mongos.
    const fpHangAgg = [];
    shardsWithForeignColl.forEach(shard => {
        fpHangAgg.push(configureFailPoint(
            st.shard1, "hangAfterAcquiringCollectionCatalog", {collection: foreignColl}));
    });

    // Start the aggregation in another thread.
    const runAggRequest =
        (mongosConn, dbName, collName, foreignCollName, sessionId, txnNum) => {
            let mongos = new Mongo(mongosConn);
            const lsid = eval("(" + sessionId + ")");
            let aggCmd = {
                aggregate: collName,
                pipeline: [
                    {
                        $lookup:
                            {from: foreignCollName, localField: "_id", foreignField: "x", as: "result"}
                    },
                    {$sort: {"_id": 1}},
                ],
                cursor: {},
                lsid: lsid,
                txnNumber: NumberLong(txnNum),
                stmtId: NumberInt(0),
                startTransaction: true,
                autocommit: false,
            };

            return mongos.getDB(dbName).runCommand(aggCmd);
        };
    let aggRequestThread = new Thread(
        runAggRequest, st.s.host, dbName, localColl, foreignColl, tojson(sessionId), txnNum);
    aggRequestThread.start();

    // In order to assert that mongos did not target the shards with the foreign collection itself,
    // wait to hit the failpoint on each shard with the foreign collection, then check that mongos
    // has only bumped its 'totalContactedParticipants' by 1 to account for the shard that owns
    // the "local" collection.
    const expectedMongosTargetedShards = initialMongosTxnMetrics.totalContactedParticipants + 1;
    fpHangAgg.forEach(fp => {
        fp.wait();
        let midpointTxnMetrics =
            assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
        assert.gte(midpointTxnMetrics.totalContactedParticipants, expectedMongosTargetedShards);
        fp.off();
    });

    aggRequestThread.join();

    // Abort the transaction.
    assert.commandWorked(st.s.getDB("admin").adminCommand({
        abortTransaction: 1,
        lsid: session.getSessionId(),
        txnNumber: NumberLong(txnNum),
        writeConcern: {w: "majority"},
        autocommit: false,
    }));

    // Check transaction metrics.
    const finalMongosTxnMetrics =
        assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
    const finalMongodTxnMetrics = allParticipants.map((shard) => {
        return assert.commandWorked(shard.adminCommand({serverStatus: 1})).transactions;
    });
    verifyFinalAbortedTransactionMetrics(initialMongosTxnMetrics,
                                         initialMongodTxnMetrics,
                                         finalMongosTxnMetrics,
                                         finalMongodTxnMetrics,
                                         allParticipants,
                                         expectedParticipants);
}

/* Test case 2 */
{
    jsTest.log("Testing that an additional participant is included in the implicit abort protocol");
    dropCollections(st);
    setupCollections(st);

    const shardsWithForeignColl = [st.shard1];
    const expectedParticipants = [st.shard0, st.shard1];

    const session = st.s.startSession();
    const txnNum = 1;
    const sessionId = session.getSessionId();

    const initialMongosTxnMetrics =
        assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
    const initialMongodTxnMetrics = allParticipants.map((shard) => {
        return assert.commandWorked(shard.adminCommand({serverStatus: 1})).transactions;
    });

    // Set a failpoint to cause the added participant, shard1, to fail with an error code;
    // shard0 will send the error to mongos, along with the added participant info.
    const fpShard1FailCommand = configureFailPoint(st.shard1,
                                                   "failWithErrorCodeAfterSessionCheckOut",
                                                   {errorCode: NumberInt(ErrorCodes.InternalError)},
                                                   {times: 1});

    let aggCmd = {
        aggregate: localColl,
        pipeline: [
            {$lookup: {from: foreignColl, localField: "_id", foreignField: "x", as: "result"}},
            {$sort: {"_id": 1}},
        ],
        cursor: {},
        lsid: session.getSessionId(),
        txnNumber: NumberLong(txnNum),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    };

    st.s.getDB(dbName).runCommand(aggCmd);

    fpShard1FailCommand.off();

    // Check transaction metrics.
    const finalMongosTxnMetrics =
        assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
    const finalMongodTxnMetrics = allParticipants.map((shard) => {
        return assert.commandWorked(shard.adminCommand({serverStatus: 1})).transactions;
    });
    verifyFinalAbortedTransactionMetrics(initialMongosTxnMetrics,
                                         initialMongodTxnMetrics,
                                         finalMongosTxnMetrics,
                                         finalMongodTxnMetrics,
                                         allParticipants,
                                         expectedParticipants);
}

/* Test case 3 */
{
    jsTest.log(
        "Testing that an additional participant not reported to mongos is not included in abort " +
        "protocol and the additional participant is reaped after transactionLifetimeLimitSeconds.");
    dropCollections(st);

    const numDocs = 500;

    //
    // In this test, mongos targets shard0 and shard1 when executing the aggregation pipeline.
    // - shard0 does not need to add another participant. shard1 adds shard2 as participant.
    // - shard0 is paused until shard1 adds shard2 as a participant.
    // - shard1 is paused from responding so that mongos never learns that shard2 was added.
    // - the transaction is aborted, causing mongos to send abortTransaction to shard0 and shard1.
    // - shard2 aborts the transaction after transactionLifetimeLimitSeconds.
    //
    // Create a sharded collection, "foo", with the following chunks:
    // shard0: _id: [-inf, 0)
    // shard1: _id: [0, +inf)
    assert.commandWorked(st.s.adminCommand({shardCollection: localNs, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: localNs, middle: {_id: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: localNs, find: {_id: -10}, to: st.shard0.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: localNs, find: {_id: 0}, to: st.shard1.shardName}));

    // Create a sharded collection, "bar", with the following chunks:
    // shard0: x: [-inf, 0)
    // shard2: x: [0, +inf)
    assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: foreignNs, middle: {x: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: foreignNs, find: {x: -10}, to: st.shard0.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: foreignNs, find: {x: 0}, to: st.shard2.shardName}));

    // These forced refreshes are not strictly necessary; they just prevent extra TXN log lines
    // from the shards starting, aborting, and restarting the transaction due to needing to
    // refresh after the transaction has started.
    [st.shard0, st.shard1, st.shard2].forEach(shard => {
        assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: localNs}));
    });
    st.refreshCatalogCacheForNs(st.s, localNs);

    [st.shard0, st.shard1, st.shard2].forEach(shard => {
        assert.commandWorked(shard.adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));
    });
    st.refreshCatalogCacheForNs(st.s, foreignNs);

    // Insert documents for shard0:
    const bulkFoo = st.s.getDB(dbName)[localColl].initializeUnorderedBulkOp();
    const bulkBar = st.s.getDB(dbName)[foreignColl].initializeUnorderedBulkOp();
    for (let ii = 1; ii <= numDocs; ++ii) {
        bulkFoo.insert({_id: -ii});
        bulkBar.insert({_id: -ii, x: -ii});
    }
    const resultFooLoad = bulkFoo.execute();
    assert.commandWorked(resultFooLoad);
    assert.eq(numDocs, resultFooLoad.nInserted);
    assert.eq(numDocs, st.s.getDB(dbName)[localColl].find().itcount());
    const resultBarLoad = bulkBar.execute();
    assert.commandWorked(resultBarLoad);
    assert.eq(numDocs, resultBarLoad.nInserted);
    assert.eq(numDocs, st.s.getDB(dbName)[foreignColl].find().itcount());
    // Insert documents for shard1:
    assert.commandWorked(st.s.getDB(dbName).foo.insert([{_id: 10}]));
    // Insert documents for shard2:
    assert.commandWorked(st.s.getDB(dbName).bar.insert([{_id: 1, x: 10}]));

    const session = st.s.startSession();
    const txnNum = 1;
    const sessionId = session.getSessionId();

    const expectedParticipants = [st.shard0, st.shard1];

    const initialMongosTxnMetrics =
        assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
    const initialMongodTxnMetrics = allParticipants.map((shard) => {
        return assert.commandWorked(shard.adminCommand({serverStatus: 1})).transactions;
    });

    // Set a failpoint on shard1 so that after adding shard2 for the aggregate command, it does not
    // respond to mongos.
    const fpHangShard1 =
        configureFailPoint(st.shard1, "hangWhenSubRouterHandlesResponseFromAddedParticipant");
    // Set a failpoint on shard0 so that it does not respond to mongos for the aggregate command
    // until after shard1 has hung.
    const fpHangShard0 = configureFailPoint(st.shard0, "getMoreHangAfterPinCursor");

    const waitForAbort = new CountDownLatch(2);

    // Start the aggregation in another thread.
    const runAggRequest =
        (mongosConn, dbName, collName, foreignCollName, sessionId, txnNum, latch) => {
            let mongos = new Mongo(mongosConn);
            const lsid = eval("(" + sessionId + ")");
            let aggCmd = {
                aggregate: collName,
                pipeline: [
                    {
                        $lookup:
                            {from: foreignCollName, localField: "_id", foreignField: "x", as:
"result"}
                    },
                    { $limit: NumberInt(200) },
                ],
                cursor: { },
                lsid: lsid,
                txnNumber: NumberLong(txnNum),
                stmtId: NumberInt(0),
                startTransaction: true,
                autocommit: false,
            };

            assert.commandWorked(mongos.getDB(dbName).runCommand(aggCmd));
            latch.countDown();

            // Abort the transaction.
            const result = assert.commandWorked(mongos.getDB("admin").runCommand({
                abortTransaction: 1,
                writeConcern: {w: "majority"},
                lsid: lsid,
                txnNumber: NumberLong(txnNum),
                stmtId: NumberInt(0),
                autocommit: false,
            }));
            latch.countDown();
            return result;
        };
    let aggRequestThread = new Thread(runAggRequest,
                                      st.s.host,
                                      dbName,
                                      localColl,
                                      foreignColl,
                                      tojson(sessionId),
                                      txnNum,
                                      waitForAbort);
    aggRequestThread.start();

    // Wait until shard1 has added shard2 as a participant.
    fpHangShard1.wait();

    // Allow shard0 to proceed; aggregation pipeline will finish without results from shard1.
    fpHangShard0.off();

    // Wait until aggregation is complete without responses from shard1 and shard2.
    assert.soon(() => {
        return waitForAbort.getCount() < 2;
    });

    // Allow shard1 to proceed; any response for the aggregation will be ignored by mongos.
    fpHangShard1.off();

    // Wait until txn is aborted.
    assert.soon(() => {
        return waitForAbort.getCount() == 0;
    });

    aggRequestThread.join();

    // Get and verify transaction metrics, including that all shards aborted.
    const finalMongosTxnMetrics =
        assert.commandWorked(st.s.adminCommand({serverStatus: 1})).transactions;
    let finalMongodTxnMetrics = allParticipants.map((shard) => {
        return assert.commandWorked(shard.adminCommand({serverStatus: 1})).transactions;
    });

    // shard2 should not have participated in the txn abort
    verifyFinalAbortedTransactionMetrics(initialMongosTxnMetrics,
                                         initialMongodTxnMetrics,
                                         finalMongosTxnMetrics,
                                         finalMongodTxnMetrics,
                                         allParticipants,
                                         expectedParticipants);

    // Wait for more than customTransactionLifetimeLimitSeconds to ensure shard2 aborts the txn.
    sleep(customTransactionLifetimeLimitSeconds * 2 * 1000);

    // Now shard2 should have aborted due to timeout.
    finalMongodTxnMetrics = allParticipants.map((shard) => {
        return assert.commandWorked(shard.adminCommand({serverStatus: 1})).transactions;
    });
    verifyFinalAbortedTransactionMetrics(initialMongosTxnMetrics,
                                         initialMongodTxnMetrics,
                                         finalMongosTxnMetrics,
                                         finalMongodTxnMetrics,
                                         allParticipants,
                                         allParticipants);
}

st.stop();
