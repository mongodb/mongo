/**
 * Tests that write operations are accepted and result in correct indexing behavior for each phase
 * of hybrid index builds.
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

let conn = MongoRunner.runMongod();
let testDB = conn.getDB('test');

let totalDocs = 0;
let crudOpsForPhase = function(coll, phase) {
    let bulk = coll.initializeUnorderedBulkOp();

    // Create 1000 documents in a specific range for this phase.
    for (let i = 0; i < 1000; i++) {
        bulk.insert({i: (phase * 1000) + i});
    }
    totalDocs += 1000;

    if (phase <= 0) {
        assert.commandWorked(bulk.execute());
        return;
    }

    // Update 50 documents.
    // For example, if phase is 2, documents [100, 150) will be updated to [-100, -150).
    let start = (phase - 1) * 100;
    for (let j = start; j < (100 * phase) - 50; j++) {
        bulk.find({i: j}).update({$set: {i: -j}});
    }
    // Delete 25 documents.
    // Similarly, if phase is 2, documents [150, 200) will be removed.
    for (let j = start + 50; j < 100 * phase; j++) {
        bulk.find({i: j}).remove();
    }
    totalDocs -= 50;

    assert.commandWorked(bulk.execute());
};

crudOpsForPhase(testDB.hybrid, 0);
assert.eq(totalDocs, testDB.hybrid.count());

// Hang the build after the first document.
const collScanFailPoint = configureFailPoint(
    testDB, "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", {fieldsToMatch: {i: 1}});

// Start the background build.
let bgBuild = startParallelShell(function() {
    assert.commandWorked(db.hybrid.createIndex({i: 1}, {background: true}));
}, conn.port);

checkLog.containsJson(conn, 20386, {
    where: "before",
    doc: function(doc) {
        return doc.i === 1;
    }
});

// Phase 1: Collection scan and external sort
// Insert documents while doing the bulk build.
crudOpsForPhase(testDB.hybrid, 1);
assert.eq(totalDocs, testDB.hybrid.count());

// Enable pause after bulk dump into index.
const insertDumpFailPoint = configureFailPoint(testDB, "hangAfterIndexBuildDumpsInsertsFromBulk");

// Wait for the bulk insert to complete.
collScanFailPoint.off();
insertDumpFailPoint.wait();

// Phase 2: First drain
// Do some updates, inserts and deletes after the bulk builder has finished.

// Hang after yielding
const yieldFailPoint = configureFailPoint(
    testDB, "hangDuringIndexBuildDrainYield", {namespace: testDB.hybrid.getFullName()});

// Enable pause after first drain.
const firstDrainFailPoint = configureFailPoint(testDB, "hangAfterIndexBuildFirstDrain");

crudOpsForPhase(testDB.hybrid, 2);
assert.eq(totalDocs, testDB.hybrid.count());

// Allow first drain to start.
insertDumpFailPoint.off();

// Ensure the operation yields during the drain, then attempt some operations.
yieldFailPoint.wait();
assert.commandWorked(testDB.hybrid.insert({i: "during yield"}));
assert.commandWorked(testDB.hybrid.remove({i: "during yield"}));
yieldFailPoint.off();

// Wait for first drain to finish.
firstDrainFailPoint.wait();

// Phase 3: Second drain
// Enable pause after second drain.
const secondDrainFailPoint = configureFailPoint(testDB, "hangAfterIndexBuildSecondDrain");

// Add inserts that must be consumed in the second drain.
crudOpsForPhase(testDB.hybrid, 3);
assert.eq(totalDocs, testDB.hybrid.count());

// Allow second drain to start.
firstDrainFailPoint.off();

// Wait for second drain to finish.
secondDrainFailPoint.wait();

// Phase 4: Final drain and commit.
// Add inserts that must be consumed in the final drain.
crudOpsForPhase(testDB.hybrid, 4);
assert.eq(totalDocs, testDB.hybrid.count());

// Allow final drain to start.
secondDrainFailPoint.off();

// Wait for build to complete.
bgBuild();

assert.eq(totalDocs, testDB.hybrid.count());
assert.commandWorked(testDB.hybrid.validate({full: true}));

MongoRunner.stopMongod(conn);
})();
