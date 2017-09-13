/**
 * Test to ensure that the basic two phase drop behavior for collections on replica sets works
 * properly.
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

    // Create the collection that will be dropped.
    twoPhaseDropTest.createCollection(collName);

    // PREPARE collection drop.
    twoPhaseDropTest.prepareDropCollection(collName);

    // COMMIT collection drop.
    twoPhaseDropTest.commitDropCollection(collName);

    twoPhaseDropTest.stop();
}());
