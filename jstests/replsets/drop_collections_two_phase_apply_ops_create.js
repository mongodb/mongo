/**
 * Test to ensure that using applyOps to create a collection, using a UUID that belongs to a
 * drop-pending collection, is a no-op. The existing collection remains in a drop-pending state.
 * This is the same behavior as trying to creating a collection with the same name as an existing
 * collection.
 */

(function() {
    "use strict";

    load("jstests/replsets/libs/two_phase_drops.js");  // For TwoPhaseDropCollectionTest.

    // Set up a two phase drop test.
    let testName = "drop_collection_two_phase_apply_ops_create";
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
        // Create collection using applyOps with UUID that belongs to a drop-pending collection.
        const dropPendingColl = twoPhaseDropTest.collectionIsPendingDrop(collName);
        const dropPendingCollName = dropPendingColl.name;
        const primary = replTest.getPrimary();
        const cmdNs = dbName + '.$cmd';
        const dropPendingCollUuid = dropPendingColl.info.uuid;
        const applyOpsCmdWithUuid = {
            applyOps: [{
                op: 'c',
                ns: cmdNs,
                ui: dropPendingCollUuid,
                o: {create: 'ignored_collection_name'}
            }]
        };
        TwoPhaseDropCollectionTest._testLog(
            'Attempting to create collection using applyOps with UUID: ' +
            tojson(applyOpsCmdWithUuid));
        assert.commandWorked(primary.adminCommand(applyOpsCmdWithUuid));
        const dropPendingCollInfo = twoPhaseDropTest.collectionIsPendingDrop(collName);
        assert(dropPendingCollInfo,
               'applyOps using UUID ' + dropPendingCollUuid +
                   ' changed drop-pending state on collection unexpectedly');
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
