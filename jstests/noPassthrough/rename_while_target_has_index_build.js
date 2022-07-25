/**
 * This tests a minimal fix for v4.2 where it is possible for rename collection to try to drop an
 * index on the existing target collection (dropTarget:true) without checking for active index
 * builds first, crashing the server because the mongod invariants against dropping an index while
 * there's an active build on that collection. The test needs to create an index name with length
 * that just meets the FCV 4.0 limit so that the correct code path triggers.
 *
 * --nojournal is not allowed in a replica set.
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

// Need replica set mode so that replication renames dropped collections to the side before
// eventually deleting them when rollback of the drop command becomes no longer possible.
const rst = new ReplSetTest({nodes: 2, nodeOptions: {enableMajorityReadConcern: "false"}});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB(jsTestName());

const sourceColl = testDB.getCollection('long_index_rename_source');
const targetColl = testDB.getCollection('long_index_rename_target');
sourceColl.drop();
targetColl.drop();
sourceColl.save({s: 20});
targetColl.save({t: 20});

/**
 * Create an index on the target collection with the longest name allowed for that collection. This
 * will ensure that the rename collection will try to specially drop the index before renaming the
 * collection to the side for drop: rename to a dropped namespace adds a special system namespace
 * prefix, which would increase the name size beyond the supported limit.
 */

const maxNsLength = 127;
const maxIndexNameLength = maxNsLength - (targetColl.getFullName() + ".$").length;
jsTestLog('Max index name length under FCV 4.0 with collection name ' + targetColl.getFullName() +
          ' = ' + maxIndexNameLength);

IndexBuildTest.pauseIndexBuilds(primary);

jsTestLog("Starting an index build on the target namespace");
const createIdx = IndexBuildTest.startIndexBuild(
    primary, targetColl.getFullName(), {a: 1}, {name: 'a'.repeat(maxIndexNameLength)});
jsTestLog("Waiting for the index build to hang");
IndexBuildTest.waitForIndexBuildToScanCollection(
    testDB, targetColl.getName(), 'a'.repeat(maxIndexNameLength));

/**
 * Ensure that the rename command now gets stopped by the in-progress index build and errors.
 */

jsTestLog("Attempting to rename a collection to a target namespace with an active index build");

TestData.sourceColl = sourceColl.getFullName();
TestData.targetColl = targetColl.getFullName();

// Using a parallel shell to get around the testing environment's implicit retries on
// BackgroundOperationInProgressForNamespace errors.
const joinRename = startParallelShell(() => {
    assert.commandFailedWithCode(
        db.adminCommand(
            {renameCollection: TestData.sourceColl, to: TestData.targetColl, dropTarget: true}),
        ErrorCodes.BackgroundOperationInProgressForNamespace);
}, primary.port);
joinRename();

/**
 * Finish the index build and then check that the rename cmd drops the index before renaming the
 * collection aside for drop. This ensures that the previously failed rename cmd was exercising the
 * correct codepath.
 */

jsTestLog("Resuming index builds and waiting for the index build to complete");
IndexBuildTest.resumeIndexBuilds(primary);
createIdx();

jsTestLog("Now that the index build is finished, rename to the target namespace should succeed");
assert.commandWorked(primary.adminCommand(
    {renameCollection: sourceColl.getFullName(), to: targetColl.getFullName(), dropTarget: true}));

checkLog.containsWithCount(
    primary, "would be too long after drop-pending rename. Dropping index immediately.", 1);

rst.stopSet();
})();
