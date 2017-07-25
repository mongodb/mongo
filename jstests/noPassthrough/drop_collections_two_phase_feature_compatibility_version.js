/**
 * Test to ensure that drop pending collections are dropped upon clean shutdown under FCV 3.4 but
 * retained under FCV 3.6.
 *
 * This test does not work with non-persistent storage engines because it checks for the presence of
 * drop-pending collections across server restarts.
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    load("jstests/replsets/libs/two_phase_drops.js");  // For TwoPhaseDropCollectionTest.

    // Set feature compatibility version on the given node. Note that setting FCV requires a
    // majority write to work so replication to secondaries must be enabled.
    function setFCV(node, featureCompatibilityVersion) {
        assert.commandWorked(
            node.adminCommand({setFeatureCompatibilityVersion: featureCompatibilityVersion}));
        let res = node.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.commandWorked(
            res, "failed to set feature compatibility version to " + featureCompatibilityVersion);
        assert.eq(featureCompatibilityVersion, res.featureCompatibilityVersion);
    }

    // Restart the primary of the given ReplSetTest.
    function restartPrimary(replTest) {
        let primaryId = replTest.getNodeId(replTest.getPrimary());
        replTest.restart(primaryId);
    }

    // Set up a two phase drop test.
    let testName = "drop_collection_two_phase";
    let dbName = testName;
    let twoPhaseDropTest = new TwoPhaseDropCollectionTest(testName, dbName);

    // Initialize replica set.
    let replTest = twoPhaseDropTest.initReplSet();

    //
    // [FCV 3.4]
    // Create a collection, put it into drop pending state, and then restart primary node under FCV
    // 3.4. Drop-pending collection should NOT be present after node comes back up.
    //
    let collToDrop34 = "collectionToDrop34";
    twoPhaseDropTest.createCollection(collToDrop34);

    jsTestLog("Setting FCV=3.4 on the primary before collection drop.");
    setFCV(replTest.getPrimary(), "3.4");
    twoPhaseDropTest.prepareDropCollection(collToDrop34);

    jsTestLog("Restarting the primary.");
    restartPrimary(replTest);

    assert(!twoPhaseDropTest.collectionIsPendingDrop(collToDrop34),
           "Collection was not removed on clean shutdown when FCV is 3.4.");

    // Resume oplog application so that we can set FCV again.
    twoPhaseDropTest.resumeOplogApplication(replTest.getSecondary());

    //
    // [FCV 3.6]
    // Create a collection, put it into drop pending state, and then restart primary node under FCV
    // 3.6. Drop-pending collection should be present after node comes back up.
    //
    let collToDrop36 = "collectionToDrop36";

    jsTestLog("Creating collection " + collToDrop36 + " on primary.");
    twoPhaseDropTest.createCollection(collToDrop36);

    jsTestLog("Setting FCV=3.6 on the primary before collection drop.");
    setFCV(replTest.getPrimary(), "3.6");
    twoPhaseDropTest.prepareDropCollection(collToDrop36);

    jsTestLog("Restarting the primary.");
    restartPrimary(replTest);

    assert(twoPhaseDropTest.collectionIsPendingDrop(collToDrop36),
           "Collection was removed on clean shutdown when FCV is 3.6.");

    // Let the secondary apply the collection drop operation, so that the replica set commit point
    // will advance, and the 'Commit' phase of the collection drop will complete on the primary.
    twoPhaseDropTest.commitDropCollection(collToDrop36);

    twoPhaseDropTest.stop();
}());
