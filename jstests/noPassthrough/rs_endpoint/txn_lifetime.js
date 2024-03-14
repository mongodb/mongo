/*
 * Tests lifetime of transactions across topology changes that make the replica set endpoint
 * become active/inactive by doing the following:
 *
 * Step 0: Create a ReplicaSetEndpointTest.
 * Step 1: Run initiateConfigServerReplicaSet(), and then start txn0. This is a non-router
 *         transaction since the replica set endpoint is still inactive.
 * Step 2: Run transitionToOneShardClusterWithConfigShard(), and then start txn1. This is a router
 *         transaction since the replica set endpoint has become active.
 * Step 3: Run transitionToTwoShardClusterWithConfigShard(), and then create txn2. This is a
 *         non-router transaction since the replica set endpoint has become inactive.
 * Step 4: Run transitionBackToOneShardClusterWithConfigShard(). This makes the replica set endpoint
 *         become active again.
 *
 * This test verifies that:
 * - After transitionToOneShardClusterWithConfigShard(), txn0 cannot be continued whether or not
 *   the user has directShardOperation privilege.
 *    - All commands other than commitTransaction fail with NoSuchTransaction which is a transient
 *      transaction error so the driver would automatically retry the transaction with a higher
 *      txnNumber on behalf of the user. (A)
 *    - The commitTransaction command fails with a non transient transaction error since it is not
 *      safe to assume that the transaction has not been committed and retry. (B)
 * - After transitionToTwoShardClusterWithConfigShard():
 *    - If the user doesn't have the directShardOperation privilege, neither txn0 nor txn1 can
 *      be continued. All commands fail with Unauthorized.
 *    - If the user has the directShardOperation privilege, both txn0 and txn1 can be continued.
 * - After transitionBackToOneShardClusterWithConfigShard(), txn1 can be continued but txn0 and txn2
 *   cannot be continued whether or not the user has directShardOperation privilege and the errors
 *   are the same as described in (A) and (B) above.
 *
 * @tags: [
 *   requires_fcv_80,
 *   featureFlagTransitionToCatalogShard
 * ]
 */
import {ReplicaSetEndpointTest} from "jstests/noPassthrough/rs_endpoint/lib/fixture.js";

function runTest(hasDirectShardOperationPrivilege) {
    const fixture = new ReplicaSetEndpointTest(hasDirectShardOperationPrivilege);

    // Step 1. The replica set endpoint is still inactive.
    fixture.initiateConfigServerReplicaSet();

    const dbName = "testDb";
    const collName = "testColl";
    const shard0TestDB = fixture.shard0AuthDB.getSiblingDB(dbName);
    const shard0TestColl = shard0TestDB.getCollection(collName);
    const docs = [{_id: ObjectId(), x: 0}, {_id: ObjectId(), x: 1}, {_id: ObjectId(), x: 2}];
    assert.commandWorked(shard0TestColl.insert(docs));

    // Start txn0 (non-router transaction).
    const txn0Opts = {
        lsid: {id: UUID()},
        txnNumber: NumberLong(5),
        autocommit: false,
    };
    const txn0DeleteCmdObj = {
        delete: collName,
        deletes: [{q: {x: 0}, limit: 1}],
        ...txn0Opts,
        startTransaction: true,
    };
    const txn0DeleteRes = shard0TestDB.runCommand(txn0DeleteCmdObj);
    assert.commandWorked(txn0DeleteRes);

    // Step 2. The replica set endpoint becomes active.
    fixture.transitionToOneShardClusterWithConfigShard();

    // Continue txn0 (non-router transaction).
    const txn0FindCmdObj = {
        find: collName,
        filter: {},
        ...txn0Opts,
    };
    const txn0FindRes0 = shard0TestDB.runCommand(txn0FindCmdObj);
    assert.commandFailedWithCode(txn0FindRes0, ErrorCodes.NoSuchTransaction);
    const txn0AbortCmdObj = {
        abortTransaction: 1,
        ...txn0Opts,
    };
    const txn0AbortTxnRes0 = shard0TestDB.adminCommand(txn0AbortCmdObj);
    assert.commandFailedWithCode(txn0AbortTxnRes0, ErrorCodes.NoSuchTransaction);
    const txn0CommitComdObj = {
        commitTransaction: 1,
        ...txn0Opts,
    };
    const txn0CommitTxnRes0 = shard0TestDB.adminCommand(txn0CommitComdObj);
    assert.commandFailedWithCode(txn0CommitTxnRes0, 50940);

    // Start txn1 (router transaction).
    const txn1Opts = {
        lsid: {id: UUID()},
        txnNumber: NumberLong(15),
        autocommit: false,
    };
    const txn1DeleteCmdObj = {
        delete: collName,
        deletes: [{q: {x: 1}, limit: 1}],
        ...txn1Opts,
        startTransaction: true,
    };
    const txn1DeleteRes = shard0TestDB.runCommand(txn1DeleteCmdObj);
    assert.commandWorked(txn1DeleteRes);

    // Step 3. The replica set endpoint becomes inactive.
    fixture.transitionToTwoShardClusterWithConfigShard();

    // Continue txn0 (non-router transaction).
    const txn0FindRes1 = shard0TestDB.runCommand(txn0FindCmdObj);
    if (hasDirectShardOperationPrivilege) {
        assert.commandWorked(txn0FindRes1);
    } else {
        assert.commandFailedWithCode(txn0FindRes1, ErrorCodes.Unauthorized);
    }

    // Continue txn1 (router transaction).
    const txn1FindCmdObj = {
        find: collName,
        filter: {},
        ...txn1Opts,
    };
    const txn1FindRes0 = shard0TestDB.runCommand(txn1FindCmdObj);
    if (hasDirectShardOperationPrivilege) {
        assert.commandWorked(txn1FindRes0);
    } else {
        assert.commandFailedWithCode(txn1FindRes0, ErrorCodes.Unauthorized);
    }

    // Start txn2 (non-router transaction).
    const txn2Opts = {
        lsid: {id: UUID()},
        txnNumber: NumberLong(25),
        autocommit: false,
    };
    const txn2DeleteCmdObj = {
        delete: collName,
        deletes: [{q: {x: 2}, limit: 1}],
        ...txn2Opts,
        startTransaction: true,
    };
    const txn2DeleteRes = shard0TestDB.runCommand(txn2DeleteCmdObj);
    if (hasDirectShardOperationPrivilege) {
        assert.commandWorked(txn2DeleteRes);
    } else {
        assert.commandFailedWithCode(txn2DeleteRes, ErrorCodes.Unauthorized);
    }

    // Step 4. The replica set endpoint becomes active again.
    fixture.transitionBackToOneShardClusterWithConfigShard();

    // Continue txn0 (non-router transaction).
    const txn0FindRes2 = shard0TestDB.runCommand(txn0FindCmdObj);
    // The error is not NoSuchTransaction because the commitTransaction command (ran in step 2
    // above) caused the transaction to be started. If we hadn't run the commitTransaction, the
    // error would be NoSuchTransaction.
    assert.commandFailedWithCode(txn0FindRes2, 8027900);

    // Continue txn1 (router transaction).
    const txn1FindRes1 = shard0TestDB.runCommand(txn1FindCmdObj);
    if (hasDirectShardOperationPrivilege) {
        assert.commandWorked(txn1FindRes1);
    } else {
        // The Unauthorized error above should cause the transaction to be implicitly aborted. So
        // the retry should fail with NoSuchTransaction.
        assert.commandFailedWithCode(txn1FindRes1, ErrorCodes.NoSuchTransaction);
    }

    // Continue txn2 (non-router transaction).
    if (txn2DeleteRes.ok) {
        const txn2FindCmdObj = {
            find: collName,
            filter: {},
            ...txn2Opts,
        };
        const txn2FindRes = shard0TestDB.runCommand(txn2FindCmdObj);
        assert.commandFailedWithCode(txn2FindRes, ErrorCodes.NoSuchTransaction);
        const txn2AbortCmdObj = {
            abortTransaction: 1,
            ...txn2Opts,
        };
        const txn2AbortTxnRes0 = shard0TestDB.adminCommand(txn2AbortCmdObj);
        assert.commandFailedWithCode(txn2AbortTxnRes0, ErrorCodes.NoSuchTransaction);
        const txn2CommitComdObj = {
            commitTransaction: 1,
            ...txn2Opts,
        };
        const txn2CommitTxnRes0 = shard0TestDB.adminCommand(txn2CommitComdObj);
        assert.commandFailedWithCode(txn2CommitTxnRes0, 50940);
    }

    fixture.tearDown();
}

runTest(true /* hasDirectShardOperationPrivilege */);
runTest(false /* hasDirectShardOperationPrivilege */)
