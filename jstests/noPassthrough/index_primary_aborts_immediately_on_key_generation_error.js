/**
 * Tests that an index build on a primary which encounters a key generation error aborts immediately
 * instead of waiting until the commit phase, while a secondary should suppress the error and
 * proceed to the next phase.
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

// Avoid optimization on empty colls. Invalid 2dsphere key.
assert.commandWorked(coll.insert(
    {geometry: {type: "Polygon", coordinates: [[[0, 0], [0, 1], [1, 1], [-2, -1], [0, 0]]]}}));

// Set the failpoint after the collection scan phase. It is expected that we never hit this
// failpoint due to the immediate index build failure. If this is hit, it will hang the test
// indefinitely.
const failpointHangAfterScan =
    configureFailPoint(testDB, "hangAfterIndexBuildDumpsInsertsFromBulk");

// Block the primary from actually aborting and replicating the 'abortIndexBuild', to give time for
// the secondary to finish the scan.
const failpointBeforeAbort = configureFailPoint(testDB, "hangIndexBuildBeforeAbortCleanUp");

// The secondary should continue to the next index build phase after suppressing the error.
const failpointHangAfterScanSecondary =
    configureFailPoint(secondaryDB, "hangAfterIndexBuildDumpsInsertsFromBulk");

// Create the index and start the build.
const createIdx = IndexBuildTest.startIndexBuild(
    primary, coll.getFullName(), {geometry: "2dsphere"}, {}, [16755], /*commitQuorum: */ 2);

// At this point the secondary has suppressed the error and will continue to the next phase.
failpointHangAfterScanSecondary.wait();
failpointHangAfterScanSecondary.off();

// Let primary finish the abort.
failpointBeforeAbort.off();

// Wait for the createIndexes command to return.
createIdx();

// The abort reason on primary must be due to the "voteAbortIndexBuild" command from the secondary.
const reasonString = `'voteAbortIndexBuild' received from '${secondary.host}'`;
checkLog.checkContainsOnceJsonStringMatch(testDB, 4656003, "error", reasonString);

// As aborting the build involves interrupting the building thread on which the user op is waiting,
// the user op will return before the primary has actually aborted the build. Waiting for the
// 'createIndexes' command to return does not guarantee that the primary has replicated the abort
// oplog entry, nor that the secondary has applied it.
IndexBuildTest.waitForIndexBuildToStop(testDB);
IndexBuildTest.waitForIndexBuildToStop(secondaryDB);

// Assert index does not exist.
IndexBuildTest.assertIndexes(coll, 1, ['_id_'], []);
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_'], []);

rst.stopSet();
})();
