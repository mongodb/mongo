/**
 * Test to ensure that dropIndexes fails on a drop-pending collection.
 * @tags: [
 *     multiversion_incompatible,
 * ]
 */

(function() {
"use strict";

load("jstests/replsets/libs/two_phase_drops.js");  // For TwoPhaseDropCollectionTest.

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

// Create the collection that will be dropped.
twoPhaseDropTest.createCollection(collName);

assert.commandWorked(coll.createIndex({a: 1}));

try {
    // PREPARE collection drop.
    const dropPendingCollName = twoPhaseDropTest.prepareDropCollection(collName);

    const dropPendingColl = testDB.getCollection(dropPendingCollName);
    assert.commandFailedWithCode(dropPendingColl.dropIndex({a: 1}), ErrorCodes.NamespaceNotFound);
} finally {
    // COMMIT collection drop.
    twoPhaseDropTest.commitDropCollection(collName);
}

twoPhaseDropTest.stop();
}());
