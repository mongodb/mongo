/**
 * Tests that initial sync resets the oldest timestamp after a failed attempt. The test will turn on
 * a failpoint that causes initial sync to fail partway through its first attempt and makes sure it
 * does not hit a WiredTiger assertion on the second attempt.
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Set the number of initial sync attempts to 2 so that the test fails on unplanned failures.
const replTest = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: "numInitialSyncAttempts=2"}});
replTest.startSet();

// Increase the election timeout to 24 hours so that we do not accidentally trigger an election
// while the secondary is restarting.
replTest.initiate();

const primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

const dbName = "test";
const collName = "initial_sync_reset_oldest_timestamp_after_failed_attempt";
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(testColl.insert({_id: 1}));

const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

// The default WC is majority and this test can't satisfy majority writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 2}));

// This will be the begin fetching point for both initial sync attempts. After the first initial
// sync attempt fails, if the oldest timestamp isn't reset before the next attempt, the update
// to the transaction table for this prepare will fail a WiredTiger assertion that the commit
// timestamp for a storage transaction cannot be older than the oldest timestamp.
const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

jsTestLog("Prepared a transaction at timestamp: " + tojson(prepareTimestamp));

replTest.stop(secondary, undefined, {skipValidation: true});
secondary = replTest.start(
    secondary,
    {
        startClean: true,
        setParameter: {
            // Set the number of operations per batch to be 1 so that we can know exactly how
            // many batches there will be.
            "replBatchLimitOperations": 1,
            "failpoint.initialSyncHangAfterDataCloning": tojson({mode: "alwaysOn"}),
            // Allow the syncing node to write the prepare oplog entry and apply the first update
            // before failing initial sync.
            "failpoint.failInitialSyncBeforeApplyingBatch": tojson({mode: {skip: 2}}),
        },
    },
    true /* wait */,
);

// Wait for failpoint to be reached so we know that collection cloning is paused.
assert.commandWorked(
    secondary.adminCommand({
        waitForFailPoint: "initialSyncHangAfterDataCloning",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

jsTestLog("Running operations while collection cloning is paused");

// This command will be in the last batch applied before the first initial sync attempt fails.
// If the oldest timestamp isn't reset on the next attempt, then the timestamp for this update
// will be the oldest timestamp.
assert.commandWorked(testColl.update({_id: 1}, {_id: 1, a: 1}));

// This entry will be applied in its own batch, so the failInitialSyncBeforeApplyingBatch
// failpoint will cause the first initial sync attempt to fail before applying this.
assert.commandWorked(testColl.update({_id: 1}, {_id: 1, b: 2}));

jsTestLog("Resuming initial sync");

assert.commandWorked(secondary.adminCommand({configureFailPoint: "initialSyncHangAfterDataCloning", mode: "off"}));

// Wait for this failpoint to be hit before turning it off and causing initial sync to fail.
assert.commandWorked(
    secondary.adminCommand({
        waitForFailPoint: "failInitialSyncBeforeApplyingBatch",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

jsTestLog("Failing first initial sync attempt");

// Turn the failpoint off and cause initial sync to fail.
assert.commandWorked(secondary.adminCommand({configureFailPoint: "failInitialSyncBeforeApplyingBatch", mode: "off"}));

replTest.awaitSecondaryNodes(null, [secondary]);

jsTestLog("Initial sync completed");

assert.commandWorked(session.abortTransaction_forTesting());

replTest.stopSet();
