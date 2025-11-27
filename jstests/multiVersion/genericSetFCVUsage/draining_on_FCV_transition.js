/**
 * Tests the draining of operations and transactions during FCV upgrade/downgrade.
 *
 * We forbid operations and transactions from spanning across FCV transitions, because otherwise
 * they may persist old format metadata after setFCV has finished cleaning up all server metadata
 * from old to new formats.
 *
 * This is achieved as follows:
 * - For unprepared transaction, we abort them.
 * - For prepared transactions, we wait for them to finish.
 * - For operations with an Operation FCV, we wait for them to finish.
 *
 * This behavior applies across all across FCV transitions (kVersion_X -> kUpgrading,
 * and kUpgrading -> kVersion_Y, kVersion_Y -> kDowngrading, and kDowngrading -> kVersion_X).
 *
 * @tags: [uses_transactions, uses_prepare_transaction, multiversion_incompatible]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";

const rst = new ReplSetTest({nodes: [{binVersion: "latest"}]});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

const dbName = "test";
const collName = jsTestName();
const testDB = primary.getDB(dbName);

/**
 * Returns true if the Server is upgrading or downgrading (i.e. on a transitional FCV).
 */
function isUpgradingOrDowngrading() {
    const fcv = assert.commandWorked(testDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
    return fcv.featureCompatibilityVersion.targetVersion !== undefined;
}

/**
 * Runs the setFCV command up to the transition to kUpgrading/kDowngrading and the corresponding
 * (first) draining of transactions via the global lock barrier.
 */
function runFCVTransitionToUpgradingOrDowngradingAndDraining(targetFCV, beforeTransitionCallback) {
    try {
        // Force setFCV to fail after the kUpgrading/kDowngrading draining,
        // but before the transition to fully upgraded/downgraded.
        assert.commandWorked(primary.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));
        assert.commandWorked(primary.adminCommand({configureFailPoint: "failUpgrading", mode: "alwaysOn"}));

        beforeTransitionCallback();

        return testDB.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true});
    } finally {
        assert(isUpgradingOrDowngrading());
        assert.commandWorked(primary.adminCommand({configureFailPoint: "failDowngrading", mode: "off"}));
        assert.commandWorked(primary.adminCommand({configureFailPoint: "failUpgrading", mode: "off"}));
    }
}

/**
 * Runs the setFCV command starting from the transition to fully upgraded/downgraded and including
 * the corresponding (second) draining of transactions via the global lock barrier.
 */
function runFCVTransitionToUpgradedOrDowngradedAndDraining(targetFCV, beforeTransitionCallback) {
    const hangBeforeUpdatingFcvDocFp = configureFailPoint(primary, "hangBeforeUpdatingFcvDoc");
    try {
        const setFcvThread = new Thread(
            function (host, targetFCV) {
                const conn = new Mongo(host);
                return conn.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true});
            },
            primary.host,
            targetFCV,
        );
        setFcvThread.start();
        hangBeforeUpdatingFcvDocFp.wait();
        assert(isUpgradingOrDowngrading());

        beforeTransitionCallback();

        hangBeforeUpdatingFcvDocFp.off();
        setFcvThread.join();
        assert(!isUpgradingOrDowngrading());

        return setFcvThread.returnData();
    } finally {
        hangBeforeUpdatingFcvDocFp.off();
    }
}

function runAbortUnpreparedTransactionsTest(initialFCV, targetFCV, runSetFCVFn) {
    jsTestLog(`Starting abort prepared transactions test from ${initialFCV} to ${targetFCV}.`);
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    jsTestLog(`Set the initial featureCompatibilityVersion to ${initialFCV}.`);
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: initialFCV, confirm: true}));

    const sessionOptions = {causalConsistency: false};
    const session = testDB.getMongo().startSession(sessionOptions);
    const sessionDB = session.getDatabase(dbName);

    assert.commandWorkedOrFailedWithCode(
        runSetFCVFn(targetFCV, function beforeTransition() {
            jsTestLog("Start a transaction.");
            session.startTransaction({readConcern: {level: "snapshot"}});
            assert.commandWorked(sessionDB[collName].insert({_id: "insert-1"}));

            jsTestLog("Attempt to drop the collection. This should fail due to the open transaction.");
            assert.commandFailedWithCode(
                testDB.runCommand({drop: collName, maxTimeMS: 1000}),
                ErrorCodes.MaxTimeMSExpired,
            );
        }),
        // setFCV returns those errors when it hits the failUpgrading/failDowngrading fail points
        // after completing the first draining. This happens here since unprepared TXNs are aborted,
        // so the draining completes and these failpoints are hit.
        [549180, 549181],
    );

    jsTestLog("Drop the collection. This should succeed, since the transaction was aborted.");
    assert.commandWorked(testDB.runCommand({drop: collName}));

    jsTestLog("Test that committing the transaction fails, since it was aborted.");
    assert.commandFailedWithCode(session.commitTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

    jsTestLog("Restore the featureCompatibilityVersion to latest.");
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    session.endSession();
    testDB[collName].drop({writeConcern: {w: "majority"}});
}

function runAwaitPreparedTransactionsTest(initialFCV, targetFCV, runSetFCVFn) {
    jsTestLog(`Starting await prepared transactions test from ${initialFCV} to ${targetFCV}.`);
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    jsTestLog(`Set the initial featureCompatibilityVersion to ${initialFCV}.`);
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: initialFCV, confirm: true}));

    const session = testDB.getMongo().startSession();
    const sessionDB = session.getDatabase(dbName);

    let prepareTimestamp;
    try {
        assert.commandFailedWithCode(
            runSetFCVFn(targetFCV, function beforeTransition() {
                jsTestLog("Start a transaction.");
                session.startTransaction();
                assert.commandWorked(sessionDB[collName].insert({"a": 1}));

                jsTestLog("Put that transaction into a prepared state.");
                prepareTimestamp = PrepareHelpers.prepareTransaction(session);

                // The setFCV command will need to acquire a global S lock to complete. The global
                // lock is currently held by prepare, so that will block. We use a failpoint to make that
                // command fail immediately when it tries to get the lock.
                assert.commandWorked(
                    primary.adminCommand({configureFailPoint: "failNonIntentLocksIfWaitNeeded", mode: "alwaysOn"}),
                );
            }),
            ErrorCodes.LockTimeout,
        );
    } finally {
        assert.commandWorked(primary.adminCommand({configureFailPoint: "failNonIntentLocksIfWaitNeeded", mode: "off"}));
    }

    jsTestLog("Commit the prepared transaction.");
    assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

    jsTestLog("Restore the featureCompatibilityVersion to latest.");
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    session.endSession();
    testDB[collName].drop({writeConcern: {w: "majority"}});
}

function runAwaitOperationsWithOFCV(initialFCV, targetFCV, runSetFCVFn) {
    jsTestLog(`Starting await for operations with Operation FCV test from ${initialFCV} to ${targetFCV}.`);

    jsTestLog(`Set the initial featureCompatibilityVersion to ${initialFCV}.`);
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: initialFCV, confirm: true}));

    let hangCreateFp, createThread;
    try {
        assert.commandFailedWithCode(
            runSetFCVFn(targetFCV, function beforeTransition() {
                jsTestLog("Start a transaction.");
                // Start creating a collection, but hang it before it acquires locks
                hangCreateFp = configureFailPoint(primary, "hangCreateCollectionBeforeLockAcquisition");
                createThread = new Thread(
                    function (host, dbName, collName) {
                        const conn = new Mongo(host);
                        assert.commandWorked(conn.getDB(dbName).createCollection(collName));
                    },
                    primary.host,
                    dbName,
                    collName,
                );
                createThread.start();
                hangCreateFp.wait();

                // This fail point makes setFCV fail immediately if it has to wait for operations
                // with an Operation FCV, rather than waiting indefinitely.
                assert.commandWorked(
                    primary.adminCommand({configureFailPoint: "immediatelyTimeOutWaitForStaleOFCV", mode: "alwaysOn"}),
                );
            }),
            ErrorCodes.ExceededTimeLimit,
        );
    } finally {
        assert.commandWorked(
            primary.adminCommand({configureFailPoint: "immediatelyTimeOutWaitForStaleOFCV", mode: "off"}),
        );
        hangCreateFp?.off();
        createThread?.join();
    }

    jsTestLog("Restore the featureCompatibilityVersion to latest.");
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    testDB[collName].drop({writeConcern: {w: "majority"}});
}

function runTest(initialFCV, targetFCV) {
    runAwaitPreparedTransactionsTest(initialFCV, targetFCV, runFCVTransitionToUpgradingOrDowngradingAndDraining);
    runAwaitPreparedTransactionsTest(initialFCV, targetFCV, runFCVTransitionToUpgradedOrDowngradedAndDraining);

    runAbortUnpreparedTransactionsTest(initialFCV, targetFCV, runFCVTransitionToUpgradingOrDowngradingAndDraining);
    runAbortUnpreparedTransactionsTest(initialFCV, targetFCV, runFCVTransitionToUpgradedOrDowngradedAndDraining);

    runAwaitOperationsWithOFCV(initialFCV, targetFCV, runFCVTransitionToUpgradingOrDowngradingAndDraining);
    runAwaitOperationsWithOFCV(initialFCV, targetFCV, runFCVTransitionToUpgradedOrDowngradedAndDraining);
}

runTest(latestFCV, lastLTSFCV);
runTest(lastLTSFCV, latestFCV);
if (lastLTSFCV !== lastContinuousFCV) {
    runTest(latestFCV, lastContinuousFCV);
    runTest(lastContinuousFCV, latestFCV);
}

rst.stopSet();
