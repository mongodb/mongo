/**
 * Test to ensure that index creation fails on a drop-pending collection.
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");            // For kDefaultWaitForFailPointTimeout.
load("jstests/noPassthrough/libs/index_build.js");  // For IndexBuildTest.
load("jstests/replsets/libs/two_phase_drops.js");   // For TwoPhaseDropCollectionTest.

// Set up a two phase drop test.
let testName = "drop_collection_two_phase";
let dbName = testName;
let collName = "collToDrop";
let twoPhaseDropTest = new TwoPhaseDropCollectionTest(testName, dbName);

// Initialize replica set.
let replTest = twoPhaseDropTest.initReplSet();

// Check for 'system.drop' two phase drop support.
if (!twoPhaseDropTest.supportsDropPendingNamespaces()) {
    jsTestLog('Drop pending namespaces not supported by storage engine. Skipping test.');
    twoPhaseDropTest.stop();
    return;
}

const primary = replTest.getPrimary();
const testDB = primary.getDB(dbName);
const coll = testDB.getCollection(collName);

if (!IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    jsTestLog('Two phase index builds not enabled. Skipping test.');
    twoPhaseDropTest.stop();
    return;
}

// Create the collection that will be dropped.
twoPhaseDropTest.createCollection(collName);

// Avoid empty collection optimization for index builds.
assert.commandWorked(coll.insert({a: 1}));

// We do not expect index builds to be able to add entries to the catalog for a drop-pending
// collection. Therefore, this should have no effect unless the index build somehow gets past the
// drop-pending namespace check.
IndexBuildTest.pauseIndexBuilds(primary);

// Pause createIndexes command before it adds the index to the catalog.
const createIndexesFailPoint =
    configureFailPoint(primary, 'hangCreateIndexesBeforeStartingIndexBuild');

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

try {
    createIndexesFailPoint.wait(30 * 1000);

    // PREPARE collection drop.
    twoPhaseDropTest.prepareDropCollection(collName);

    // The IndexBuildsCoordinator will not proceed with the index build on the drop-pending
    // collection and fail with a NamespaceNotFound error. The createIndexes command will ignore
    // this error and return success to the caller. This is consistent with the current treatment
    // for index builds on dropped collections.
    createIndexesFailPoint.off();
    createIdx();

    // COMMIT collection drop.
    twoPhaseDropTest.commitDropCollection(collName);
} finally {
    createIndexesFailPoint.off();
    IndexBuildTest.resumeIndexBuilds(primary);
}

twoPhaseDropTest.stop();
}());
