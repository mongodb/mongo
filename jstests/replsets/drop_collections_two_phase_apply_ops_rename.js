/**
 * Test to ensure that using applyOps to rename a drop-pending collection is a no-op. The collection
 * remains in a drop-pending state. This is the same behavior as renaming a non-existent collection.
 */

(function() {
    "use strict";

    load("jstests/replsets/libs/two_phase_drops.js");  // For TwoPhaseDropCollectionTest.

    // Set up a two phase drop test.
    let testName = "drop_collection_two_phase_apply_ops_rename";
    let dbName = testName;
    let collName = "collToDrop";
    let twoPhaseDropTest = new TwoPhaseDropCollectionTest(testName, dbName);

    // Initialize replica set.
    let replTest = twoPhaseDropTest.initReplSet();

    // Create the collection that will be dropped.
    twoPhaseDropTest.createCollection(collName);

    // PREPARE collection drop.
    twoPhaseDropTest.prepareDropCollection(collName);

    try {
        // Rename drop-pending collection using applyOps with system.drop namespace.
        const dropPendingColl = twoPhaseDropTest.collectionIsPendingDrop(collName);
        const dropPendingCollName = dropPendingColl.name;
        const primary = replTest.getPrimary();
        const cmdNs = dbName + '.$cmd';
        const sourceNs = dbName + '.' + dropPendingCollName;
        const destNs = dbName + '.bar';
        const applyOpsCmdWithName = {
            applyOps: [{op: 'c', ns: cmdNs, o: {renameCollection: sourceNs, to: destNs}}]
        };
        TwoPhaseDropCollectionTest._testLog(
            'Attempting to rename collection using applyOps with system.drop namespace: ' +
            tojson(applyOpsCmdWithName));
        assert.commandWorked(primary.adminCommand(applyOpsCmdWithName));
        assert(twoPhaseDropTest.collectionIsPendingDrop(collName),
               'applyOps using collection name ' + dropPendingCollName +
                   ' renamed drop-pending collection unexpectedly');
        assert(!twoPhaseDropTest.collectionExists(collName));

        // Rename drop-pending collection using applyOps with UUID.
        const dropPendingCollUuid = dropPendingColl.info.uuid;
        const applyOpsCmdWithUuid = {
            applyOps: [{
                op: 'c',
                ns: cmdNs,
                ui: dropPendingCollUuid,
                o: {renameCollection: dbName + '.ignored_collection_name', to: destNs}
            }]
        };
        TwoPhaseDropCollectionTest._testLog(
            'Attempting to rename collection using applyOps with UUID: ' +
            tojson(applyOpsCmdWithUuid));
        assert.commandWorked(primary.adminCommand(applyOpsCmdWithUuid));
        const dropPendingCollInfo = twoPhaseDropTest.collectionIsPendingDrop(collName);
        assert(dropPendingCollInfo,
               'applyOps using UUID ' + dropPendingCollUuid +
                   ' renamed drop-pending collection unexpectedly');
        assert.eq(dropPendingCollUuid.hex(),
                  dropPendingCollInfo.info.uuid.hex(),
                  'drop pending collection UUID does not match UUID of original collection: ' +
                      tojson(dropPendingCollInfo));

        // COMMIT collection drop.
        twoPhaseDropTest.commitDropCollection(collName);
    } finally {
        twoPhaseDropTest.stop();
    }
}());
