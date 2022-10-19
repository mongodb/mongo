/**
 * Test that we allow collections and databases to be dropped in standalone mode with unfinished
 * replicated index builds.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/disk/libs/wt_file_helper.js');
load('jstests/noPassthrough/libs/index_build.js');

const dbName = jsTestName();
const collName1 = "test1";
const collName2 = "test2";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const secondary = rst.getSecondary();
let secondaryDB = secondary.getDB(dbName);
const secondaryDbpath = secondary.dbpath;

const primaryColl1 = primary.getDB(dbName).getCollection(collName1);
const primaryColl2 = primary.getDB(dbName).getCollection(collName2);
assert.commandWorked(primaryColl1.insert({_id: 0, a: 1}));
assert.commandWorked(primaryColl2.insert({_id: 0, a: 1}));

jsTestLog("Starting index builds on primary and pausing before completion");
IndexBuildTest.pauseIndexBuilds(primary);
const createIdx1 = IndexBuildTest.startIndexBuild(primary, primaryColl1.getFullName(), {a: 1});
const createIdx2 = IndexBuildTest.startIndexBuild(primary, primaryColl2.getFullName(), {a: 1});

// Waiting for secondary to start the index builds
IndexBuildTest.waitForIndexBuildToStart(secondaryDB, collName1);
IndexBuildTest.waitForIndexBuildToStart(secondaryDB, collName2);

jsTestLog("Shutting down secondary");
rst.stop(secondary);

//  Waiting for parallel index build threads to finish
let exitCode = createIdx1({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to shutdown');
exitCode = createIdx2({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to shutdown');

jsTestLog("Starting secondary as standalone");
const mongod = MongoRunner.runMongod({dbpath: secondaryDbpath, noReplSet: true, noCleanData: true});
secondaryDB = mongod.getDB(dbName);

// Confirm that the secondary node leaves the index on coll1 as unfinished.
IndexBuildTest.assertIndexes(
    secondaryDB.getCollection(collName1), 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true});

jsTestLog("Dropping collection from secondary");
assert.commandWorked(secondaryDB.runCommand({drop: collName1}));

// Confirm that the secondary node leaves the index on coll2 as unfinished so we can check that
// dropping database is also able to drop collections and indexes.
IndexBuildTest.assertIndexes(
    secondaryDB.getCollection(collName2), 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true});

jsTestLog("Dropping database from secondary");
assert.commandWorked(secondaryDB.dropDatabase());

MongoRunner.stopMongod(mongod);
rst.stopSet();
})();
