/*
 * Tests that an index build fails gracefully as it is interrupted just before it signals that it
 * is ready to commit by updating the corresponding document in config.system.indexBuilds.
 *
 * @tags: [
 *     requires_document_locking,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('coll');
const indexBuildsColl = primary.getCollection('config.system.indexBuilds');

assert.commandWorked(coll.insert({_id: 1, a: 1}));

// Enable fail point which makes index build hang before it reads the index build
const failPoint = configureFailPoint(primary, 'hangBeforeGettingIndexBuildEntry');

const createIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {}, [
    ErrorCodes.IndexBuildAborted,
    ErrorCodes.NoMatchingDocument
]);

failPoint.wait();

// Index build should be present in the config.system.indexBuilds collection.
const indexMap =
    IndexBuildTest.assertIndexes(coll, 2, ['_id_'], ['a_1'], {includeBuildUUIDs: true});
const indexBuildUUID = indexMap['a_1'].buildUUID;
assert(indexBuildsColl.findOne({_id: indexBuildUUID}));

// Abort the index build. It will remove the document for the index build from the
// config.system.indexBuilds collection.
jsTestLog('Aborting the index build');
const abortIndexThread = startParallelShell('assert.commandWorked(db.getMongo().getCollection("' +
                                                coll.getFullName() + '").dropIndex("a_1"))',
                                            primary.port);
checkLog.containsJson(primary, 4656010);

// Unblock the index build and wait for the threads to join.
failPoint.off();

abortIndexThread();

// Index build should be removed from the config.system.indexBuilds collection.
assert.isnull(indexBuildsColl.findOne({_id: indexBuildUUID}));

createIndex();

jsTestLog('Waiting for index build to complete');
IndexBuildTest.waitForIndexBuildToStop(testDB, coll.getName(), 'a_1');

IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

rst.stopSet();
})();
