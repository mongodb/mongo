/**
 * Tests that a bypassDocumentValidation write in progress when a collMod sets
 * prepareConstraintValidationLevel is able to complete. The collMod must wait for the collection
 * X lock, but writes that already hold the IX lock must not be starved or killed.
 *
 * @tags: [
 *   requires_fcv_90,
 *   featureFlagConstraintValidationLevel,
 *   uses_parallel_shell,
 *   # configureFailPoint requires direct mongod access.
 *   assumes_against_mongod_not_mongos,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const testDb = db.getSiblingDB("prepare_constraint_validation_level_failpoint_tests");
const collName = jsTestName();
const validator = {
    a: {$exists: true},
};
testDb.runCommand({drop: collName});
assert.commandWorked(testDb.createCollection(collName, {validator, validationLevel: "strict"}));

const port = db.getMongo().port;
const dbName = testDb.getName();

// Pause the write after acquiring the collection IX lock but before writing.
const fp = configureFailPoint(db, "hangWithLockDuringBatchInsert");

// Step 1: start a non-conforming bypass write in a parallel shell; it will pause at the failpoint.
const awaitWrite = startParallelShell(
    funWithArgs(
        (dbName, collName) => {
            assert.commandWorked(
                db.getSiblingDB(dbName).runCommand({
                    insert: collName,
                    documents: [{b: 1}],
                    bypassDocumentValidation: true,
                }),
            );
        },
        dbName,
        collName,
    ),
    port,
);

// Step 2: wait for the write to pause (it now holds the collection IX lock).
fp.wait();

// Step 3: start the collMod in a parallel shell; it will wait for the X lock.
const awaitCollMod = startParallelShell(
    funWithArgs(
        (dbName, collName) => {
            assert.commandWorked(
                db
                    .getSiblingDB(dbName)
                    .runCommand({collMod: collName, prepareConstraintValidationLevel: true}),
            );
        },
        dbName,
        collName,
    ),
    port,
);

// Wait until the collMod is visible in currentOp, meaning it is queued for the X lock.
assert.soon(() => {
    return (
        db
            .getSiblingDB("admin")
            .aggregate([{$currentOp: {}}, {$match: {"command.collMod": collName}}])
            .toArray().length > 0
    );
});

// Step 4: start a second bypass write. The collMod X lock is already queued, so this write will
// be blocked behind it. Once the collMod completes and sets the flag, this write will fail.
const awaitWriteAfter = startParallelShell(
    funWithArgs(
        (dbName, collName) => {
            assert.commandFailed(
                db.getSiblingDB(dbName).runCommand({
                    insert: collName,
                    documents: [{b: 2}],
                    bypassDocumentValidation: true,
                }),
            );
        },
        dbName,
        collName,
    ),
    port,
);

// Step 5: release the failpoint so the first write can complete.
fp.off();

// The first write (which acquired the IX lock before the flag was set) and the collMod succeed.
// The second write (which acquired the IX lock after the flag was set) fails.
awaitWrite();
awaitCollMod();
awaitWriteAfter();

testDb.runCommand({drop: collName});
