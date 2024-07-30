/**
 * Tests TemporarilyUnavailable errors during transaction application on a secondary, both
 * prepared or not, are converted to WCE and retried.
 *
 * @tags: [
 *   does_not_support_config_fuzzer,
 *   requires_persistence,
 *   requires_non_retryable_writes,
 *   requires_wiredtiger,
 *   uses_transactions,
 *   # TODO(SERVER-30987): remove this JS test after unit test is available.
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

function checkTemporarilyUnavailableRetriedOnSecondary(rst, isPrepared) {
    jsTestLog("checkTemporarilyUnavailableRetriedOnSecondary: isPrepared=" + isPrepared);

    const primary = rst.getPrimary();
    const db = primary.getDB(jsTestName());
    const coll = db[jsTestName()];

    // Prepare collection. Add an index to increase the load to dirty more cache.
    coll.drop();
    assert.commandWorked(coll.createIndex({x: "text"}));

    // Reduce secondary cache size to trigger TransactionTooLargeForCache errors.
    const secondary = rst.getSecondary();
    const secondaryDb = secondary.getDB(jsTestName());
    assert.commandWorked(secondaryDb.adminCommand(
        {setParameter: 1, "wiredTigerEngineRuntimeConfig": "cache_size=16M"}));

    const serverStatusBefore = secondaryDb.serverStatus();
    const convertedToWriteConflictBefore =
        serverStatusBefore.metrics.operation.temporarilyUnavailableErrorsConvertedToWriteConflict;
    jsTestLog("temporarilyUnavailableErrorsConvertedToWriteConflict before commit: " +
              convertedToWriteConflictBefore);

    // Start and commit transaction in a parallel shell. The transaction commit will block waiting
    // for the secondary to commit, so to check the server is behaving as expected we need to do the
    // commit in parallel.
    async function doCommitTransaction(dbName, collName, isPrepared) {
        const {PrepareHelpers} = await import("jstests/core/txns/libs/prepare_helpers.js");

        const insertDoc = {x: []};
        for (var j = 0; j < 50000; j++) {
            insertDoc.x.push("" + Math.random() + Math.random());
        }

        const session = db.getMongo().startSession();
        const sessionDb = session.getDatabase(dbName);
        const sessionColl = sessionDb[collName];

        // Retry the transaction until we eventually succeed.
        assert.soon(() => {
            session.startTransaction();
            let result;
            try {
                result = sessionColl.insert(insertDoc);
                assert.writeOK(result);
            } catch (e) {
                session.abortTransaction();
                assert.commandFailedWithCode(result, [
                    ErrorCodes.WriteConflict,
                    ErrorCodes.TemporarilyUnavailable,
                    ErrorCodes.TransactionTooLargeForCache
                ]);
                return false;
            }

            if (isPrepared) {
                const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
                PrepareHelpers.commitTransaction(session, prepareTimestamp);
            } else {
                assert.commandWorked(session.commitTransaction_forTesting());
            }
            return true;
        }, "Expected a transaction to eventually succeed.");
    }
    const commitTransactionShell = startParallelShell(
        funWithArgs(doCommitTransaction, db.getName(), jsTestName(), isPrepared), primary.port);

    // Expect the secondary temporarilyUnavailableErrorsConvertedToWriteConflict to increase.
    const targetConvertedToWriteConflict = convertedToWriteConflictBefore + 5;
    assert.soon(() => {
        const serverStatusNow = secondaryDb.serverStatus();
        const convertedToWriteConflict =
            serverStatusNow.metrics.operation.temporarilyUnavailableErrorsConvertedToWriteConflict;
        jsTestLog("temporarilyUnavailableErrorsConvertedToWriteConflict now: " +
                  convertedToWriteConflict);
        return convertedToWriteConflict >= targetConvertedToWriteConflict;
    });

    // Restore cache size to default and allow operation to complete.
    assert.commandWorked(secondaryDb.adminCommand(
        {setParameter: 1, "wiredTigerEngineRuntimeConfig": "cache_size=1G"}));

    commitTransactionShell();
}

// Set up replica set with 2 nodes.
const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
            setParameter: {
                // Disable TransactionTooLargeForCache, all cache pressure errors result in TUE.
                transactionTooLargeForCacheThreshold: 1
            }
        },
    ]
});

rst.startSet();
rst.initiate();

checkTemporarilyUnavailableRetriedOnSecondary(rst, /*isPrepared=*/ false);
checkTemporarilyUnavailableRetriedOnSecondary(rst, /*isPrepared=*/ true);

rst.stopSet();