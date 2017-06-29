/**
 * Test to ensure that initial sync builds indexes correctly when syncing a capped collection that
 * is receiving concurrent inserts.
 *
 * The main goal of this test is to have the SECONDARY clone more documents than would actually fit
 * in a specific capped collection, leading to the deletion of documents (i.e. 'capping') on the
 * SECONDARY *during* the collection cloning process. This scenario is encountered when a SECONDARY
 * opens a cursor on a capped collection, begins iterating on that cursor, and, before the cursor is
 * exhausted, new documents get appended to the capped collection that it is cloning.
 *
 * Test Setup:
 * 1-node replica set that is reconfigured to a 2-node replica set.
 *
 * 1. Initiate replica set.
 * 2. Create a capped collection on the PRIMARY and overflow it.
 * 4. Add a SECONDARY node to the replica set.
 * 5. Set fail point on SECONDARY that hangs capped collection clone after first 'find' response.
 * 6. Let SECONDARY start initial sync.
 * 7. Wait for initial 'find' response during the cloning of the capped collection.
 * 8. Insert documents to the capped collection on the PRIMARY.
 * 9, Disable fail point on SECONDARY so the rest of the capped collection documents are cloned.
 * 8. Once initial sync completes, ensure that capped collection indexes on the SECONDARY are valid.
 *
 * This is a regression test for SERVER-29197. This test does not need to be run on 3.2 initial sync
 * since the bug described in SERVER-29197 only applies to the initial sync logic in versions >=
 * 3.4.
 *
 * @tags: [requires_3dot4_initial_sync]
 *
 */
(function() {
    "use strict";

    load("jstests/libs/check_log.js");

    /**
     * Overflow a capped collection 'coll' by continuously inserting a given document,
     * 'docToInsert'.
     */
    function overflowCappedColl(coll, docToInsert) {
        // Insert one document and save its _id.
        assert.writeOK(coll.insert(docToInsert));
        var origFirstDocId = coll.findOne()["_id"];
        var bulkBatchSize = 4;

        // Insert documents in batches, since it's faster that way. Detect overflow by seeing if the
        // original first doc of the collection is still present.
        while (coll.findOne({_id: origFirstDocId})) {
            var bulk = coll.initializeUnorderedBulkOp();
            for (var i = 0; i < bulkBatchSize; i++) {
                bulk.insert(docToInsert);
            }
            assert.writeOK(bulk.execute());
        }
    }

    // Set up replica set.
    var testName = "initial_sync_capped_index";
    var dbName = testName;
    var replTest = new ReplSetTest({name: testName, nodes: 1});
    replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();
    var primaryDB = primary.getDB(dbName);
    var cappedCollName = "capped_coll";
    var primaryCappedColl = primaryDB[cappedCollName];

    // Create the capped collection. We make the capped collection large enough so that the initial
    // sync collection cloner will need more than one cursor batch to fetch all documents.
    // TODO: Once the collection cloner batch size is configurable, we can reduce collection &
    // document size to make this test more lightweight.
    var MB = 1024 * 1024;
    var cappedCollSize = 48 * MB;

    jsTestLog("Creating capped collection of size " + (cappedCollSize / MB) + " MB.");
    assert.commandWorked(
        primaryDB.createCollection(cappedCollName, {capped: true, size: cappedCollSize}));

    // Overflow the capped collection.
    jsTestLog("Overflowing the capped collection.");

    var docSize = 4 * MB;
    var largeDoc = {a: new Array(docSize).join("*")};
    overflowCappedColl(primaryCappedColl, largeDoc);

    // Add a SECONDARY node.
    jsTestLog("Adding secondary node.");
    replTest.add();

    var secondary = replTest.getSecondary();
    var collectionClonerFailPoint = "initialSyncHangCollectionClonerAfterInitialFind";

    // Make the collection cloner pause after its initial 'find' response on the capped collection.
    var nss = dbName + "." + cappedCollName;
    jsTestLog("Enabling collection cloner fail point for " + nss);
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: collectionClonerFailPoint, mode: 'alwaysOn', data: {nss: nss}}));

    // Let the SECONDARY begin initial sync.
    jsTestLog("Re-initiating replica set with new secondary.");
    replTest.reInitiate();

    jsTestLog("Waiting for the initial 'find' response of capped collection cloner to complete.");
    checkLog.contains(
        secondary, "initialSyncHangCollectionClonerAfterInitialFind fail point enabled for " + nss);

    // Append documents to the capped collection so that the SECONDARY will clone these
    // additional documents.
    var docsToAppend = 2;
    for (var i = 0; i < docsToAppend; i++) {
        assert.writeOK(primaryDB[cappedCollName].insert(largeDoc));
    }

    // Let the 'getMore' requests for the capped collection clone continue.
    jsTestLog("Disabling collection cloner fail point for " + nss);
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: collectionClonerFailPoint, mode: 'off', data: {nss: nss}}));

    // Wait until initial sync completes.
    replTest.awaitReplication();

    // Make sure the indexes created during initial sync are valid.
    var secondaryCappedColl = secondary.getDB(dbName)[cappedCollName];
    var validate_result = secondaryCappedColl.validate(true);
    var failMsg =
        "Index validation of '" + secondaryCappedColl.name + "' failed: " + tojson(validate_result);
    assert(validate_result.valid, failMsg);

})();
