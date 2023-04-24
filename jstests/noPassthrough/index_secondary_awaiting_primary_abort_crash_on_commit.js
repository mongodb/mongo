/**
 * When an index build encounters an error, it signals the primary to abort and waits for an abort
 * oplog entry to be replicated. If a commit entry is received instead, the secondary should crash.
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load('jstests/libs/fail_point_util.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
const secondaryColl = secondaryDB.getCollection('test');

// Avoid optimization on empty colls.
assert.commandWorked(coll.insert({a: 1}));

// Pause the index builds on the secondary, using the 'hangAfterStartingIndexBuild' failpoint.
const failpointHangAfterInit = configureFailPoint(secondaryDB, "hangAfterInitializingIndexBuild");

// Block the primary before commit.
const failpointHangBeforeCommitPrimary = configureFailPoint(testDB, "hangIndexBuildBeforeCommit");

// Create the index and start the build. Set commitQuorum of 1 node explicitly, we want the primary
// to commit even if the secondary is failing.
const createIdx = IndexBuildTest.startIndexBuild(primary,
                                                 coll.getFullName(),
                                                 {a: 1},
                                                 {},
                                                 [ErrorCodes.InterruptedDueToReplStateChange],
                                                 /*commitQuorum: */ 1);

failpointHangAfterInit.wait();

// Extract the index build UUID. Use assertIndexesSoon to retry until the oplog applier is done with
// the entry, and the index is visible to listIndexes. The failpoint does not ensure this.
const buildUUID =
    IndexBuildTest
        .assertIndexesSoon(secondaryColl, 2, ['_id_'], ['a_1'], {includeBuildUUIDs: true})['a_1']
        .buildUUID;

const failSecondaryBuild =
    configureFailPoint(secondaryDB,
                       "failIndexBuildWithError",
                       {buildUUID: buildUUID, error: ErrorCodes.OutOfDiskSpace});

// Hang while in kAwaitPrimaryAbort, but before signaling the primary.
const failpointHangBeforeSignalingAbort =
    configureFailPoint(secondaryDB, "hangIndexBuildBeforeSignalingPrimaryForAbort");

// Unblock index builds, causing the failIndexBuildWithError failpoint to throw an error.
failpointHangAfterInit.off();

// Unblock primary commit.
failpointHangBeforeCommitPrimary.off();

let res;
assert.soon(function() {
    res = checkProgram(secondary.pid);
    return !res.alive;
});

assert.eq(MongoRunner.EXIT_ABORT, res.exitCode);

// Expect the secondary to crash. Depending on timing, this can be either because the secondary was
// waiting for a primary abort when a 'commitIndexBuild' is applied, or because the build fails and
// tries to request an abort while a 'commitIndexBuild' is being applied.
assert(rawMongoProgramOutput().match('Fatal assertion.*(7329403|7329407)'),
       'Receiving a commit from the primary for a failing index build should crash the secondary');

createIdx();

// Assert index exists on the primary.
IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1']);

TestData.skipCheckDBHashes = true;
rst.stopSet();
})();
