/**
 * Test to ensure that using applyOps to drop a drop-pending collection is a no-op.
 * By definition, a drop-pending collection will be removed by the server eventually.
 */

(function() {
    "use strict";

    load("jstests/replsets/libs/two_phase_drops.js");  // For TwoPhaseDropCollectionTest.

    // Set up a two phase drop test.
    let testName = "drop_collection_two_phase_apply_ops_noop";
    let dbName = testName;
    let collName = "collToDrop";
    let twoPhaseDropTest = new TwoPhaseDropCollectionTest(testName, dbName);

    // Initialize replica set.
    let replTest = twoPhaseDropTest.initReplSet();

    // Create the collection that will be dropped.
    twoPhaseDropTest.createCollection(collName);

    // PREPARE collection drop.
    twoPhaseDropTest.prepareDropCollection(collName);

    // Drop drop-pending collection using applyOps with system.drop namespace.
    const dropPendingColl = twoPhaseDropTest.collectionIsPendingDrop(collName);
    const dropPendingCollName = dropPendingColl.name;
    const primary = replTest.getPrimary();
    const cmdNs = dbName + '.$cmd';
    const applyOpsCmdWithName = {applyOps: [{op: 'c', ns: cmdNs, o: {drop: dropPendingCollName}}]};
    TwoPhaseDropCollectionTest._testLog(
        'Attempting to drop collection using applyOps with system.drop namespace: ' +
        tojson(applyOpsCmdWithName));
    assert.commandWorked(primary.adminCommand(applyOpsCmdWithName));
    assert(twoPhaseDropTest.collectionIsPendingDrop(collName),
           'applyOps using collection name ' + dropPendingCollName +
               ' removed drop-pending collection unexpectedly');

    // Drop drop-pending collection using applyOps with UUID.
    const dropPendingCollUuid = dropPendingColl.info.uuid;
    const applyOpsCmdWithUuid = {
        applyOps:
            [{op: 'c', ns: cmdNs, ui: dropPendingCollUuid, o: {drop: 'ignored_collection_name'}}]
    };
    TwoPhaseDropCollectionTest._testLog('Attempting to drop collection using applyOps with UUID: ' +
                                        tojson(applyOpsCmdWithUuid));
    assert.commandWorked(primary.adminCommand(applyOpsCmdWithUuid));
    assert(twoPhaseDropTest.collectionIsPendingDrop(collName),
           'applyOps using UUID ' + dropPendingCollUuid +
               ' removed drop-pending collection unexpectedly');

    // COMMIT collection drop.
    twoPhaseDropTest.commitDropCollection(collName);

    twoPhaseDropTest.stop();
}());
