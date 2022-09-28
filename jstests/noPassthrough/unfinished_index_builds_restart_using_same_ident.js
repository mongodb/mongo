/**
 * Tests that during startup recovery unfinished index builds that are not resumable will drop and
 * recreate the index table using the same ident to avoid doing untimestamped writes to the catalog.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 *     requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/noPassthrough/libs/index_build.js');

const replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();

const dbName = "test";
const collName = jsTestName();

let primary = replSet.getPrimary();
let primaryDB = primary.getDB(dbName);
let primaryColl = primaryDB[collName];

assert.commandWorked(primaryDB.createCollection(collName));
assert.commandWorked(primaryColl.insert({x: 1}));
assert.commandWorked(primaryColl.insert({x: 2}));
assert.commandWorked(primaryColl.insert({x: 3}));

const failPoint = configureFailPoint(primaryDB, "hangIndexBuildBeforeCommit");
const indexBuild = IndexBuildTest.startIndexBuild(primaryDB.getMongo(),
                                                  primaryColl.getFullName(),
                                                  {x: 1},
                                                  {},
                                                  [ErrorCodes.InterruptedAtShutdown]);
failPoint.wait();

// Get the index ident.
const ident = assert.commandWorked(primaryDB.runCommand({collStats: collName}))
                  .indexDetails.x_1.uri.substring('statistics:table:'.length);
jsTestLog("Ident: " + ident);

// Take a checkpoint so that the unfinished index is present in the catalog during the next startup.
assert.commandWorked(primary.adminCommand({fsync: 1}));

// Crash and restart the node.
replSet.stop(primary, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
indexBuild({checkExitSuccess: false});
replSet.start(primary, {noCleanData: true, setParameter: {logLevel: 1}});

primary = replSet.getPrimary();
primaryDB = primary.getDB(dbName);
primaryColl = primaryDB[collName];

// Resetting unfinished index.
checkLog.containsJson(
    primary, 6987700, {namespace: primaryColl.getFullName(), index: "x_1", ident: ident});

// WT drop.
checkLog.containsJson(primary, 22338, {uri: "table:" + ident});

// Create uri.
checkLog.containsJson(primary, 51780, {uri: "table:" + ident});

// Index build starting.
checkLog.containsJson(primary, 20384, {ident: ident});

IndexBuildTest.waitForIndexBuildToStop(primaryDB);
IndexBuildTest.assertIndexes(primaryColl, 2, ["_id_", "x_1"]);

assert.commandWorked(primaryColl.insert({x: 4}));
assert.commandWorked(primaryColl.insert({x: 5}));

assert.eq(5, primaryColl.find().hint("x_1").count());

replSet.stopSet();
}());
