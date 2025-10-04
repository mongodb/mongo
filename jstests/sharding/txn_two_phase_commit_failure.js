/**
 * Exercises two-phase commit failures.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction, multiversion_incompatible]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    flushRoutersAndRefreshShardMetadata,
    getCoordinatorFailpoints,
} from "jstests/sharding/libs/sharded_transactions_helpers.js";
import {runCommitThroughMongos} from "jstests/sharding/libs/txn_two_phase_commit_util.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

// Lower the transaction timeout for participants, since this test exercises the case where the
// coordinator hangs before writing the participant list and then checks that the transaction is
// eventually aborted on the coordinator, and the coordinator will only abort on reaching the
// transaction timeout.
TestData.transactionLifetimeLimitSeconds = 30;

let lsid = {id: UUID()};
let txnNumber = 0;

(function () {
    let st = new ShardingTest({
        shards: 3,
        rs0: {nodes: [{}]},
        causallyConsistent: true,
        other: {mongosOptions: {verbose: 3}},
    });

    let coordinatorReplSetTest = st.rs0;

    let participant0 = st.shard0;
    let participant1 = st.shard1;
    let participant2 = st.shard2;

    const setUp = function () {
        // Create a sharded collection with a chunk on each shard:
        // shard0: [-inf, 0)
        // shard1: [0, 10)
        // shard2: [10, +inf)
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: participant0.shardName}));
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
        assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
        assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));

        flushRoutersAndRefreshShardMetadata(st, {ns});

        // Start a new transaction by inserting a document onto each shard.
        assert.commandWorked(
            st.s.getDB(dbName).runCommand({
                insert: collName,
                documents: [{_id: -5}, {_id: 5}, {_id: 15}],
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                stmtId: NumberInt(0),
                startTransaction: true,
                autocommit: false,
            }),
        );
    };

    const testCommitProtocol = function (failpointData, expectError = ErrorCodes.NoSuchTransaction) {
        jsTest.log("Testing commit protocol with failpointData: " + tojson(failpointData));

        txnNumber++;
        setUp();

        let coordPrimary = coordinatorReplSetTest.getPrimary();

        assert.commandWorked(
            coordPrimary.adminCommand({
                configureFailPoint: failpointData.failpoint,
                mode: {skip: failpointData.skip ? failpointData.skip : 0},
                data: failpointData.options ? failpointData.options : {},
            }),
        );

        runCommitThroughMongos(extractUUIDFromObject(lsid.id), txnNumber, st.s.host, expectError);

        // Check that the transaction aborted as expected.
        jsTest.log("Verify that the transaction was aborted on all shards.");
        assert.eq(0, st.s.getDB(dbName).getCollection(collName).find().itcount());

        st.s.getDB(dbName).getCollection(collName).drop();
        clearRawMongoProgramOutput();

        assert.commandWorked(coordPrimary.adminCommand({configureFailPoint: failpointData.failpoint, mode: "off"}));
    };

    //
    // Run through all the failpoints. Each failpoint is targeting different error handling block.
    //

    // This triggers timeout in the chain registered in TransactionCoordinator constructor, which throws
    // TransactionCoordinatorReachedAbortDecision caught by the onError() in the same chain.
    testCommitProtocol(
        getCoordinatorFailpoints().find((data) => data.failpoint === "hangBeforeWritingParticipantList"),
    );

    // This is one of the standard error codes that a transaction shard can generate and is supported by
    // the transaction coordinator directly.
    testCommitProtocol({
        failpoint: "failRemoteTransactionCommand",
        numTimesShouldBeHit: 2,
        options: {command: "prepareTransaction", code: ErrorCodes.NoSuchTransaction},
    });

    // Similar to above, except with different error code, which is expected to be propagated to the
    // abort response.
    testCommitProtocol(
        {
            failpoint: "failRemoteTransactionCommand",
            numTimesShouldBeHit: 2,
            options: {command: "prepareTransaction", code: ErrorCodes.TransactionTooOld},
        },
        ErrorCodes.TransactionTooOld,
    );

    // This is one of the non standard error codes from a transaction shard, it is retried by the
    // per-shard retry logic and it is eventually converted into
    // TransactionCoordinatorReachedAbortDecision and is caught by the onError block in the
    // sendPrepareToShard() helper method.
    testCommitProtocol({
        failpoint: "failRemoteTransactionCommand",
        numTimesShouldBeHit: 2,
        options: {command: "prepareTransaction", code: ErrorCodes.CommandNotFound},
    });

    st.stop();
})();
