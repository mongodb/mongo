/**
 * Test to ensure that it is OK to run convertToCapped using applyOps on a drop-pending collection.
 */

(function() {
    "use strict";

    load("jstests/replsets/libs/two_phase_drops.js");  // For TwoPhaseDropCollectionTest.

    // Set up a two phase drop test.
    let testName = "drop_collection_two_phase_apply_ops_convert_to_capped";
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
        // Converting a drop-pending collection to a capped collection returns NamespaceNotFound.
        const dropPendingColl = twoPhaseDropTest.collectionIsPendingDrop(collName);
        const dropPendingCollName = dropPendingColl.name;
        const primary = replTest.getPrimary();
        const convertToCappedCmdWithName = {
            convertToCapped: dropPendingCollName,
            size: 100000,
        };
        TwoPhaseDropCollectionTest._testLog(
            'Attempting to convert collection with system.drop namespace: ' +
            tojson(convertToCappedCmdWithName));
        assert.commandFailedWithCode(primary.getDB(dbName).runCommand(convertToCappedCmdWithName),
                                     ErrorCodes.NamespaceNotFound);
        let dropPendingCollInfo = twoPhaseDropTest.collectionIsPendingDrop(collName);
        assert(dropPendingCollInfo,
               'convertToCapped using collection name ' + dropPendingCollName +
                   ' affected drop-pending collection state unexpectedly');
        assert(!dropPendingCollInfo.options.capped);
        assert(!twoPhaseDropTest.collectionExists(collName));

        // Converting a drop-pending collection to a capped collection using applyOps with
        // system.drop namespace.
        const cmdNs = dbName + '.$cmd';
        const applyOpsCmdWithName = {
            applyOps:
                [{op: 'c', ns: cmdNs, o: {convertToCapped: dropPendingCollName, size: 100000}}]
        };
        TwoPhaseDropCollectionTest._testLog(
            'Attempting to convert collection using applyOps with system.drop namespace: ' +
            tojson(applyOpsCmdWithName));
        assert.commandFailedWithCode(primary.adminCommand(applyOpsCmdWithName),
                                     ErrorCodes.UnknownError);
        assert(twoPhaseDropTest.collectionIsPendingDrop(collName),
               'applyOps using collection name ' + dropPendingCollName +
                   ' affected drop-pending collection state unexpectedly');

        // Converting a drop-pending collection to a capped collection using applyOps with UUID.
        const dropPendingCollUuid = dropPendingColl.info.uuid;
        const applyOpsCmdWithUuid = {
            applyOps: [{
                op: 'c',
                ns: cmdNs,
                ui: dropPendingCollUuid,
                o: {convertToCapped: 'ignored_collection_name', size: 100000}
            }]
        };
        TwoPhaseDropCollectionTest._testLog(
            'Attempting to convert collection using applyOps with UUID: ' +
            tojson(applyOpsCmdWithUuid));
        assert.commandFailedWithCode(primary.adminCommand(applyOpsCmdWithUuid),
                                     ErrorCodes.UnknownError);
        dropPendingCollInfo = twoPhaseDropTest.collectionIsPendingDrop(collName);
        assert(dropPendingCollInfo,
               'applyOps using UUID ' + dropPendingCollUuid +
                   ' affected drop-pending collection state unexpectedly');
        assert(!dropPendingCollInfo.options.capped);
        assert.eq(dropPendingCollUuid.hex(),
                  dropPendingCollInfo.info.uuid.hex(),
                  'drop pending collection UUID does not match UUID of original collection: ' +
                      tojson(dropPendingCollInfo));
        assert(!twoPhaseDropTest.collectionExists(collName));

        // COMMIT collection drop.
        twoPhaseDropTest.commitDropCollection(collName);
    } finally {
        twoPhaseDropTest.stop();
    }
}());
