/**
 * Test to ensure that computation of dbHash ignores drop pending collections, when performing a two
 * phase collection drop.
 */

(function() {
    'use strict';

    load("jstests/replsets/libs/two_phase_drops.js");  // For TwoPhaseDropCollectionTest.

    // Compute db hash for all collections on given database.
    function getDbHash(database) {
        let res =
            assert.commandWorked(database.runCommand({dbhash: 1}), "'dbHash' command failed.");
        return res.md5;
    }

    // Set up a two phase drop test.
    let testName = "drop_collection_two_phase_long_index_names";
    let dbName = testName;
    let collName = "collToDrop";
    let twoPhaseDropTest = new TwoPhaseDropCollectionTest(testName, dbName);

    // Initialize replica set.
    let replTest = twoPhaseDropTest.initReplSet();
    let primaryDB = replTest.getPrimary().getDB(dbName);

    // Create the collection that will be dropped.
    twoPhaseDropTest.createCollection(collName);

    // Save the dbHash while drop is in 'pending' state.
    twoPhaseDropTest.prepareDropCollection(collName);
    let dropPendingDbHash = getDbHash(primaryDB);

    // Save the dbHash after the drop has been committed.
    twoPhaseDropTest.commitDropCollection(collName);
    let dropCommittedDbHash = getDbHash(primaryDB);

    // The dbHash calculation should ignore drop pending collections. Therefore, therefore, the hash
    // during prepare phase and commit phase should match.
    let failMsg = "dbHash during drop pending phase did not match dbHash after drop was committed.";
    assert.eq(dropPendingDbHash, dropCommittedDbHash, failMsg);

    replTest.stopSet();

})();