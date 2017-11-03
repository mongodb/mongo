/**
 * Test to ensure that the two phase drop behavior is applied to the target collection of a
 * renameCollection command when dropTarget is set to true.
 */

(function() {
    'use strict';

    load('jstests/replsets/libs/two_phase_drops.js');  // For TwoPhaseDropCollectionTest.
    load('jstests/libs/check_log.js');                 // For checkLog.contains().

    // Set up a two phase drop test.
    let testName = 'drop_collection_two_phase_rename_drop_target';
    let dbName = testName;
    let fromCollName = 'collToRename';
    let toCollName = 'collToDrop';
    let twoPhaseDropTest = new TwoPhaseDropCollectionTest(testName, dbName);

    // Initialize replica set.
    let replTest = twoPhaseDropTest.initReplSet();

    // Create the collections that will be renamed and dropped.
    twoPhaseDropTest.createCollection(fromCollName);
    twoPhaseDropTest.createCollection(toCollName);

    // Insert documents into both collections so that we can tell them apart.
    const primary = replTest.getPrimary();
    const testDb = primary.getDB(dbName);
    const fromColl = testDb.getCollection(fromCollName);
    const toColl = testDb.getCollection(toCollName);
    assert.writeOK(fromColl.insert({_id: 'from'}));
    assert.writeOK(toColl.insert({_id: 'to'}));
    replTest.awaitReplication();

    // Prevent renameCollection from being applied on the secondary so that we can examine the state
    // of the primary after target collection has been dropped.
    jsTestLog('Pausing oplog application on the secondary node.');
    const secondary = replTest.getSecondary();
    twoPhaseDropTest.pauseOplogApplication(secondary);

    // This logs each operation being applied.
    const previousLogLevel =
        assert.commandWorked(primary.setLogLevel(1, 'storage')).was.replication.verbosity;

    try {
        // When the target collection exists, the renameCollection command should fail if dropTarget
        // flag is set to false or is omitted.
        jsTestLog(
            'Checking renameCollection error handling when dropTarget is set to false and target collection exists.');
        let dropTarget = false;
        assert.commandFailedWithCode(fromColl.renameCollection(toCollName, dropTarget),
                                     ErrorCodes.NamespaceExists);

        // Rename collection with dropTarget set to true. Check collection contents after rename.
        jsTestLog('Renaming collection ' + fromColl.getFullName() + ' to ' + toColl.getFullName() +
                  ' with dropTarget set to true.');
        dropTarget = true;
        assert.commandWorked(fromColl.renameCollection(toColl.getName(), dropTarget));
        assert(!twoPhaseDropTest.collectionExists(fromCollName));
        assert(twoPhaseDropTest.collectionExists(toCollName));
        assert.eq({_id: 'from'}, toColl.findOne());

        // Confirm that original target collection is now a drop-pending collection.
        assert(twoPhaseDropTest.collectionIsPendingDrop(toCollName));

        // COMMIT collection drop.
        twoPhaseDropTest.resumeOplogApplication(secondary);
        replTest.awaitReplication();
        assert.soonNoExcept(function() {
            return !twoPhaseDropTest.collectionIsPendingDrop(toCollName);
        });

        // Confirm in the logs that the renameCollection dropped the target collection on the
        // secondary using two phase collection drop.
        checkLog.contains(secondary, 'dropCollection: ' + toColl.getFullName());

        // Rename target collection back to source collection. This helps to ensure the collection
        // metadata is updated correctly on both primary and secondary.
        assert.commandWorked(toColl.renameCollection(fromCollName + '_roundtrip'));
        replTest.awaitReplication();
    } finally {
        // Reset log level.
        primary.setLogLevel(previousLogLevel, 'storage');

        twoPhaseDropTest.stop();
    }
}());
