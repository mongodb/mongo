/**
 * Tests that config.transactions documents are correctly modified on FCV upgrade/downgrade and that
 * retryability is preserved for transactions that don't use prepare after upgrade and always for
 * retryable writes.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";
    load("jstests/libs/feature_compatibility_version.js");
    load('jstests/sharding/libs/sharded_transactions_helpers.js');

    const dbName = "test";
    const collName = "config_transactions_set_fcv";

    // Define autocommit as a variable so it can be used in object literals w/o an explicit value.
    const autocommit = false;

    // Start a replica set with an odd number of members to verify nodes outside the majority behave
    // correctly around setFeatureCompatibilityVersion, which uses majority writes to update the FCV
    // document. The primary isn't expected to change, so each secondary is given priority 0.
    const rst =
        new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}]});
    rst.startSet();
    rst.initiate();

    let testDB = rst.getPrimary().getDB(dbName);
    let adminDB = rst.getPrimary().getDB("admin");

    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    // Starts a dummy transaction, commits or aborts it with or without prepare, then returns the
    // commit or abort response. Returns the response from prepare if it fails.
    function runTxn({lsid, txnNumber}, {commit, prepare, leaveOpen}) {
        const startTransactionRes = testDB.runCommand({
            insert: collName,
            documents: [{x: "dummy_txn"}],
            txnNumber: NumberLong(txnNumber),
            startTransaction: true, lsid, autocommit,
        });
        if (!startTransactionRes.ok || leaveOpen) {
            return startTransactionRes;
        }

        if (prepare) {
            const prepareRes = testDB.adminCommand(
                {prepareTransaction: 1, txnNumber: NumberLong(txnNumber), lsid, autocommit});
            if (!prepareRes.ok) {
                return prepareRes;
            }

            if (commit) {
                // Add 1 to the increment so that the commitTimestamp is after the prepareTimestamp.
                const commitTimestamp = Timestamp(prepareRes.prepareTimestamp.getTime(),
                                                  prepareRes.prepareTimestamp.getInc() + 1);
                return testDB.adminCommand({
                    commitTransaction: 1,
                    commitTimestamp,
                    txnNumber: NumberLong(txnNumber), lsid, autocommit
                });
            } else {
                return testDB.adminCommand(
                    {abortTransaction: 1, txnNumber: NumberLong(txnNumber), lsid, autocommit});
            }
        }

        if (commit) {
            return testDB.adminCommand(
                {commitTransaction: 1, txnNumber: NumberLong(txnNumber), lsid, autocommit});
        } else {
            return testDB.adminCommand(
                {abortTransaction: 1, txnNumber: NumberLong(txnNumber), lsid, autocommit});
        }
    }

    // Retries commitTransaction for the given txnId, returning the response.
    function retryCommit({lsid, txnNumber}) {
        return testDB.adminCommand(
            {commitTransaction: 1, txnNumber: NumberLong(txnNumber), lsid, autocommit});
    }

    // Asserts aborting the given txnId returns NoSuchTransaction.
    function assertTransactionAborted({lsid, txnNumber}) {
        assert.commandFailedWithCode(
            testDB.adminCommand(
                {abortTransaction: 1, txnNumber: NumberLong(txnNumber), lsid, autocommit}),
            ErrorCodes.NoSuchTransaction);
    }

    // Global counter for the number of retryable writes completed. Used to verify retried retryable
    // writes aren't double applied.
    let numRetryableWrites = 0;

    // Runs a dummy retryable write and increments the retryable write counter.
    function assertRetryableWriteWorked({lsid, txnNumber}) {
        numRetryableWrites += 1;
        assert.commandWorked(testDB.runCommand({
            insert: collName,
            documents: [{fromRetryableWrite: true}],
            txnNumber: NumberLong(txnNumber), lsid
        }));
    }

    // Verifies a txnId has already been used for a retryable write by running a dummy retryable
    // write and asserting the write isn't applied.
    function assertRetryableWriteCanBeRetried({lsid, txnNumber}) {
        assert.commandWorked(testDB.runCommand({
            insert: collName,
            documents: [{fromRetryableWrite: true}],
            txnNumber: NumberLong(txnNumber), lsid
        }));
        assert.eq(numRetryableWrites, testDB[collName].find({fromRetryableWrite: true}).itcount());
    }

    // Searches config.transactions for an entry for the given txnId on each node in the replica
    // set, verifying the entry does / does not exist and has the expected state, if specified.
    function checkConfigTransactionEntry(rst, {lsid, txnNumber}, {hasEntry, expectedState}) {
        rst.awaitReplication();
        rst.nodes.forEach((node) => {
            // Search for id since we don't know the uid, which is generated by the server.
            const entry = node.getDB("config").transactions.findOne({"_id.id": lsid.id});

            if (!hasEntry) {
                // There should be no entry for this session or it should be for an earlier
                // operation.
                if (entry) {
                    assert.gt(txnNumber,
                              entry.txnNum,
                              "expected entry to have lower txnNumber, entry: " + tojson(entry) +
                                  ", node: " + tojson(node));
                } else {
                    assert.isnull(entry,
                                  "expected entry to be null, entry: " + tojson(entry) +
                                      ", node: " + tojson(node));
                }
                return;
            }

            assert.eq(txnNumber,
                      entry.txnNum,
                      "expected entry to have the same txnNumber, entry: " + tojson(entry) +
                          ", node: " + tojson(node));

            if (expectedState) {
                assert.eq(expectedState,
                          entry.state,
                          "entry: " + tojson(entry) + ", node: " + tojson(node));
            } else {
                assert(!entry.hasOwnProperty("state"),
                       "expected entry to not have state, entry: " + tojson(entry) + ", node: " +
                           tojson(node));
            }
        });
    }

    function runTest({shouldRestart}) {
        // The test waits for failpoints to log a message when hit, so clear the program output
        // before starting so messages from previous iterations aren't in it.
        clearRawMongoProgramOutput();

        const txnIds = {
            write: {lsid: {id: UUID()}, txnNumber: 0},   // Retryable write.
            commit: {lsid: {id: UUID()}, txnNumber: 0},  // Committed transaction w/o prepare.
            commitPrepare: {lsid: {id: UUID()}, txnNumber: 0},  // Committed transaction w/ prepare.
            abort: {lsid: {id: UUID()}, txnNumber: 0},          // Aborted transaction w/o prepare.
            abortPrepare: {lsid: {id: UUID()}, txnNumber: 0},  // Aborted transaction after prepare.
            concurrentTxn: {lsid: {id: UUID()}, txnNumber: 0},  // Transaction concurrent w/ setFCV.
            concurrentWrite:
                {lsid: {id: UUID()}, txnNumber: 0},  // Retryable write concurrent w/ setFCV.
            upgradingTxn:
                {lsid: {id: UUID()}, txnNumber: 0},  // Transaction started during FCV upgrade.
        };

        //
        // In the latest FCV, verify the expected updates are made to config.transactions for each
        // case and the successful operations are retryable.
        //
        checkFCV(adminDB, latestFCV);

        assertRetryableWriteWorked(txnIds.write);
        assert.commandWorked(runTxn(txnIds.commit, {commit: true, prepare: false}));
        assert.commandWorked(runTxn(txnIds.commitPrepare, {commit: true, prepare: true}));
        assert.commandWorked(runTxn(txnIds.abort, {commit: false, prepare: false}));
        assert.commandWorked(runTxn(txnIds.abortPrepare, {commit: false, prepare: true}));

        checkConfigTransactionEntry(rst, txnIds.write, {hasEntry: true});
        checkConfigTransactionEntry(
            rst, txnIds.commit, {hasEntry: true, expectedState: "committed"});
        checkConfigTransactionEntry(
            rst, txnIds.commitPrepare, {hasEntry: true, expectedState: "committed"});
        checkConfigTransactionEntry(rst, txnIds.abort, {hasEntry: false});
        checkConfigTransactionEntry(
            rst, txnIds.abortPrepare, {hasEntry: true, expectedState: "aborted"});

        // The retryable write and the commit of both committed transactions should be retryable.
        // The aborted transactions should still be aborted.
        assertRetryableWriteCanBeRetried(txnIds.write);
        assert.commandWorked(retryCommit(txnIds.commit));
        assert.commandWorked(retryCommit(txnIds.commitPrepare));
        assertTransactionAborted(txnIds.abort);
        assertTransactionAborted(txnIds.abortPrepare);

        //
        // Downgrade to the last-stable FCV and verify config.transactions was updated as expected
        // for previously completed operations and operations concurrent with the downgrade.
        //

        if (shouldRestart) {
            // Restart to verify config.transactions entries for sessions not in-memory at the
            // beginning of FCV downgrade are updated correctly.
            jsTestLog("Restarting replica set before downgrading the featureCompatibilityVersion.");
            for (let i = 0; i < rst.nodes.length; i++) {
                rst.restart(i);
            }
            testDB = rst.getPrimary().getDB(dbName);
            adminDB = rst.getPrimary().getDB("admin");
        }

        // Make setFCV pause in the downgrading state after getting the list of sessions to
        // potentially modify.
        assert.commandWorked(rst.getPrimary().adminCommand(
            {configureFailPoint: "pauseBeforeDowngradingSessions", mode: "alwaysOn"}));

        // Downgrade FCV in a parallel shell and wait until it blocks at the failpoint above.
        const awaitDowngradeFCV = startParallelShell(() => {
            load("jstests/libs/feature_compatibility_version.js");
            jsTestLog("Downgrade the featureCompatibilityVersion in a parallel shell.");
            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
        }, rst.getPrimary().port);
        waitForFailpoint("Hit pauseBeforeDowngradingSessions failpoint", 1 /*numTimes*/);

        // Concurrent transactions that use prepare will fail.
        assert.commandFailedWithCode(runTxn(txnIds.concurrentTxn, {commit: true, prepare: true}),
                                     ErrorCodes.CommandNotSupported);
        txnIds.concurrentTxn.txnNumber += 1;

        // Concurrent transactions that do not use prepare and retryable writes succeed.
        assert.commandWorked(runTxn(txnIds.concurrentTxn, {commit: true, prepare: false}));
        assertRetryableWriteWorked(txnIds.concurrentWrite);

        // Unset the failpoint and wait for the downgrade to finish.
        assert.commandWorked(rst.getPrimary().adminCommand(
            {configureFailPoint: "pauseBeforeDowngradingSessions", mode: "off"}));

        awaitDowngradeFCV();
        checkFCV(adminDB, lastStableFCV);

        // The successful concurrent operations should have entries without state and be retryable.
        checkConfigTransactionEntry(rst, txnIds.concurrentTxn, {hasEntry: true});
        assert.commandWorked(retryCommit(txnIds.concurrentTxn));
        checkConfigTransactionEntry(rst, txnIds.concurrentWrite, {hasEntry: true});
        assertRetryableWriteCanBeRetried(txnIds.concurrentWrite);

        // Only the retryable write entry should remain.
        checkConfigTransactionEntry(rst, txnIds.write, {hasEntry: true});
        checkConfigTransactionEntry(rst, txnIds.commit, {hasEntry: false});
        checkConfigTransactionEntry(rst, txnIds.commitPrepare, {hasEntry: false});
        checkConfigTransactionEntry(rst, txnIds.abort, {hasEntry: false});
        checkConfigTransactionEntry(rst, txnIds.abortPrepare, {hasEntry: false});

        // The retryable write can be retried.
        assertRetryableWriteCanBeRetried(txnIds.write);

        // Neither of the commits can be retried.
        assert.commandFailedWithCode(retryCommit(txnIds.commit), ErrorCodes.NoSuchTransaction);
        assert.commandFailedWithCode(retryCommit(txnIds.commitPrepare),
                                     ErrorCodes.NoSuchTransaction);

        //
        // In the last-stable FCV, verify the expected updates are made to config.transactions for
        // each case and the successful operations are retryable.
        //

        // Reset each txnId to test upgrade with a clean slate.
        Object.keys(txnIds).forEach((txnIdKey) => {
            txnIds[txnIdKey].lsid = {id: UUID()};
            txnIds[txnIdKey].txnNumber = 0;
        });

        // Prepare can't be used in FCV 4.0, so only commit, abort, and retryable write should
        // succeed.
        assertRetryableWriteWorked(txnIds.write);
        assert.commandWorked(runTxn(txnIds.commit, {commit: true, prepare: false}));
        assert.commandFailedWithCode(runTxn(txnIds.commitPrepare, {commit: true, prepare: true}),
                                     ErrorCodes.CommandNotSupported);
        assert.commandWorked(runTxn(txnIds.abort, {commit: false, prepare: false}));
        assert.commandFailedWithCode(runTxn(txnIds.abortPrepare, {commit: false, prepare: true}),
                                     ErrorCodes.CommandNotSupported);

        // Only the retryable write and transaction that committed without prepare should have an
        // entry. Neither should have state.
        checkConfigTransactionEntry(rst, txnIds.write, {hasEntry: true});
        checkConfigTransactionEntry(rst, txnIds.commit, {hasEntry: true});
        checkConfigTransactionEntry(rst, txnIds.commitPrepare, {hasEntry: false});
        checkConfigTransactionEntry(rst, txnIds.abort, {hasEntry: false});
        checkConfigTransactionEntry(rst, txnIds.abortPrepare, {hasEntry: false});

        // The retryable write and successful commit can be retried.
        assertRetryableWriteCanBeRetried(txnIds.write);
        assert.commandWorked(retryCommit(txnIds.commit));

        if (shouldRestart) {
            // Restart to verify config.transactions entries for sessions not in-memory at the
            // beginning of FCV upgrade are updated correctly.
            jsTestLog("Restarting replica set before upgrading the featureCompatibilityVersion.");
            for (let i = 0; i < rst.nodes.length; i++) {
                rst.restart(i);
            }
            testDB = rst.getPrimary().getDB(dbName);
            adminDB = rst.getPrimary().getDB("admin");
        }

        //
        // Upgrade to the latest FCV and verify config.transactions was updated as expected for
        // previously completed operations and operations concurrent with the upgrade.
        //

        // Run a retryable write on the session that will be used during upgrade so it has a
        // transaction table entry and will be checked out by the upgrade.
        assertRetryableWriteWorked(txnIds.upgradingTxn);
        txnIds.upgradingTxn.txnNumber += 1;

        // Make setFCV pause in the upgrading state after getting the list of sessions to
        // potentially modify.
        assert.commandWorked(rst.getPrimary().adminCommand(
            {configureFailPoint: "pauseBeforeUpgradingSessions", mode: "alwaysOn"}));

        // Upgrade FCV in a parallel shell and wait until it blocks at the failpoint above.
        const awaitUpgradeFCV = startParallelShell(() => {
            load("jstests/libs/feature_compatibility_version.js");
            jsTestLog("Upgrade the featureCompatibilityVersion in a parallel shell.");
            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
        }, rst.getPrimary().port);
        waitForFailpoint("Hit pauseBeforeUpgradingSessions failpoint", 1 /*numTimes*/);

        // Concurrent transactions that use prepare will fail.
        assert.commandFailedWithCode(runTxn(txnIds.concurrentTxn, {commit: true, prepare: true}),
                                     ErrorCodes.CommandNotSupported);
        txnIds.concurrentTxn.txnNumber += 1;

        // Concurrent transactions that do not use prepare and retryable writes succeed.
        assert.commandWorked(runTxn(txnIds.concurrentTxn, {commit: true, prepare: false}));
        assertRetryableWriteWorked(txnIds.concurrentWrite);

        // Start a transaction in the upgrading state and verify that it doesn't get aborted by the
        // rest of the upgrade. Note that all sessions are killed and their transactions aborted for
        // writes to the FCV document except when it is set to the fully upgraded state, so this
        // can't be tested for downgrade.
        assert.commandWorked(runTxn(txnIds.upgradingTxn, {leaveOpen: true}));

        // Unset the failpoint and wait for the upgrade to finish.
        assert.commandWorked(rst.getPrimary().adminCommand(
            {configureFailPoint: "pauseBeforeUpgradingSessions", mode: "off"}));

        awaitUpgradeFCV();
        checkFCV(adminDB, latestFCV);

        // The transaction started while upgrading shouldn't have been killed and can be committed.
        assert.commandWorked(testDB.adminCommand({
            commitTransaction: 1,
            lsid: txnIds.upgradingTxn.lsid,
            txnNumber: NumberLong(txnIds.upgradingTxn.txnNumber), autocommit
        }));

        // The successful concurrent transaction should have "committed" state and be retryable, and
        // the concurrent retryable write should not have state and also be retryable.
        checkConfigTransactionEntry(
            rst, txnIds.concurrentTxn, {hasEntry: true, expectedState: "committed"});
        assert.commandWorked(retryCommit(txnIds.concurrentTxn));
        checkConfigTransactionEntry(rst, txnIds.concurrentWrite, {hasEntry: true});
        assertRetryableWriteCanBeRetried(txnIds.concurrentWrite);

        // There should still only be entries for the committed transaction and retryable write. The
        // committed transaction should now have a "state" field.
        checkConfigTransactionEntry(rst, txnIds.write, {hasEntry: true});
        checkConfigTransactionEntry(
            rst, txnIds.commit, {hasEntry: true, expectedState: "committed"});
        checkConfigTransactionEntry(rst, txnIds.commitPrepare, {hasEntry: false});
        checkConfigTransactionEntry(rst, txnIds.abort, {hasEntry: false});
        checkConfigTransactionEntry(rst, txnIds.abortPrepare, {hasEntry: false});

        // The retryable write and successful commit can be retried.
        assertRetryableWriteCanBeRetried(txnIds.write);
        assert.commandWorked(retryCommit(txnIds.commit));
    }

    runTest({shouldRestart: false});
    runTest({shouldRestart: true});

    //
    // Verify setFCV is interruptible between modifying sessions.
    //
    clearRawMongoProgramOutput();
    checkFCV(adminDB, latestFCV);

    // Construct a config.transactions entry that would be modified by downgrade.
    const txnIds = {interrupt: {lsid: {id: UUID()}, txnNumber: 0}};
    assert.commandWorked(runTxn(txnIds.interrupt, {commit: true, prepare: true}));
    checkConfigTransactionEntry(
        rst, txnIds.interrupt, {hasEntry: true, expectedState: "committed"});

    // Pause setFCV before it would modify the entry.
    assert.commandWorked(rst.getPrimary().adminCommand(
        {configureFailPoint: "pauseBeforeDowngradingSessions", mode: "alwaysOn"}));

    TestData.setFCVLsid = {id: UUID()};
    const awaitUpgradeFCV = startParallelShell(() => {
        load("jstests/libs/feature_compatibility_version.js");
        assert.commandFailedWithCode(
            db.adminCommand(
                {setFeatureCompatibilityVersion: lastStableFCV, lsid: TestData.setFCVLsid}),
            ErrorCodes.Interrupted);
    }, rst.getPrimary().port);
    waitForFailpoint("Hit pauseBeforeDowngradingSessions failpoint", 1 /*numTimes*/);

    // Kill the session running setFCV.
    assert.commandWorked(rst.getPrimary().adminCommand({killSessions: [TestData.setFCVLsid]}));

    // Unpause the failpoint and verify setFCV returns without modifying config.transactions.
    assert.commandWorked(rst.getPrimary().adminCommand(
        {configureFailPoint: "pauseBeforeDowngradingSessions", mode: "off"}));

    awaitUpgradeFCV();
    checkConfigTransactionEntry(
        rst, txnIds.interrupt, {hasEntry: true, expectedState: "committed"});

    rst.stopSet();
}());
