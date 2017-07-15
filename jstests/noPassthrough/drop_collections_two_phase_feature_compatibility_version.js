/**
 * Test to ensure that drop pending collections are not dropped upon clean shutdown under FCV 3.4
 * and FCV 3.6.
 *
 * This test does not work with non-persistent storage engines because it checks for the presence of
 * drop-pending collections across server restarts.
 * @tags: [requires_persistence]
 */

(function() {
    "use strict";

    // Return a list of all collections in a given database. Use 'args' as the
    // 'listCollections' command arguments.
    function listCollections(database, args) {
        var args = args || {};
        var failMsg = "'listCollections' command failed";
        var res = assert.commandWorked(database.runCommand("listCollections", args), failMsg);
        return res.cursor.firstBatch;
    }

    // Return a list of all collection names in a given database.
    function listCollectionNames(database, args) {
        return listCollections(database, args).map(c => c.name);
    }

    // Set a fail point on a specified node.
    function setFailPoint(node, failpoint, mode) {
        assert.commandWorked(node.adminCommand({configureFailPoint: failpoint, mode: mode}));
    }

    // Returns true if database contains drop-pending namespace corresponding to 'collToDrop'.
    function containsDropPendingCollection(database, collToDrop) {
        var collections = listCollections(database, {includePendingDrops: true});

        jsTestLog("Checking presence of drop-pending collection for " + collToDrop +
                  "  in the collection list: " + tojson(collections));

        // Make sure the collection doesn't appear in the collection list.
        assert.eq(undefined, collections.find(c => c.name === collToDrop));

        // Returns true if the collection is now in 'drop-pending' state. The collection name should
        // be of the format "system.drop.<optime>.<collectionName>", where 'optime' is the optime of
        // the collection drop operation, encoded as a string, and 'collectionName' is the original
        // collection name.
        var pendingDropRegex = new RegExp("system\.drop\..*\." + collToDrop + "$");

        return collections.find(c => pendingDropRegex.test(c.name));
    }

    // Ensures that a drop-pending collection corresponding to 'collToDrop' is present on the
    // primary.
    function createDropPendingCollection(primaryDB, secondary, collToDrop) {
        // Create the collection that will be dropped and let it replicate.
        primaryDB.createCollection(collToDrop);
        replTest.awaitReplication();

        // Pause application on secondary so that commit point doesn't advance, meaning that a
        // dropped
        // collection on the primary will remain in 'drop-pending' state.
        jsTestLog("Pausing oplog application on the secondary node.");
        setFailPoint(secondary, "rsSyncApplyStop", "alwaysOn");

        // Make sure the collection was created.
        var collNames = listCollectionNames(primaryDB);
        assert.contains(
            collToDrop, collNames, "Collection '" + collToDrop + "' wasn't created properly");

        // Drop the collection on the primary.
        jsTestLog("Dropping collection '" + collToDrop + "' on primary node.");
        assert.commandWorked(primaryDB.runCommand({drop: collToDrop, writeConcern: {w: 1}}));

        assert(containsDropPendingCollection(primaryDB, collToDrop),
               "Collection was not found in the 'system.drop' namespace");
    }

    // Set feature compatibility version and creates a drop-pending collection before restarting
    // the primary.
    function restartPrimaryWithDropPendingCollection(
        featureCompatibilityVersion, replTest, dbName, collToDrop) {
        var primary = replTest.getPrimary();
        var secondary = replTest.getSecondary();

        // Setting priority of node 1 ensures that only node 0 can be primary even across restarts.
        assert.eq(0, replTest.getNodeId(primary));

        // Setting feature compatibility requires write majority to work. Re-enable oplog
        // application on the secondary if it was disabled previously.
        setFailPoint(secondary, "rsSyncApplyStop", "off");

        assert.commandWorked(
            primary.adminCommand({setFeatureCompatibilityVersion: featureCompatibilityVersion}));
        var res = primary.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
        assert.commandWorked(
            res, "failed to set feature compatibility version to " + featureCompatibilityVersion);
        assert.eq(featureCompatibilityVersion, res.featureCompatibilityVersion);

        // Create a new collection and transition it to a drop-pending state before restarting the
        // primary.
        var primaryDB = primary.getDB(dbName);
        createDropPendingCollection(primaryDB, secondary, collToDrop);

        // Restart node 0 and wait for it to become primary again. Node 1 cannot become primary
        // because it is configured with a priority of 0.
        jsTestLog("Drop-pending collection present. Restarting primary " + primary.host +
                  " with feature compatibility version " + featureCompatibilityVersion);
        replTest.restart(0);
        primary = replTest.getPrimary();
        assert.eq(0, replTest.getNodeId(primary));
        jsTestLog("Primary " + primary.host +
                  " restarted successfully with feature compatibility version " +
                  featureCompatibilityVersion);
    }

    var replTest = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});

    // Initiate the replica set.
    replTest.startSet();
    replTest.initiate();
    replTest.awaitReplication();

    // Restart primary node under FCV 3.4. Drop-pending collection should be present after node
    // comes back up.
    var dbName = "test";
    var collToDrop34 = "collectionToDrop34";
    restartPrimaryWithDropPendingCollection("3.4", replTest, dbName, collToDrop34);
    var primary = replTest.getPrimary();
    var primaryDB = primary.getDB(dbName);
    assert(containsDropPendingCollection(primaryDB, collToDrop34),
           "Collection was removed on clean shutdown when FCV is 3.4.");

    // Restart primary node under FCV 3.6. Drop-pending collection should be present after node
    // comes back up.
    var collToDrop36 = "collectionToDrop36";
    restartPrimaryWithDropPendingCollection("3.6", replTest, dbName, collToDrop36);
    var primary = replTest.getPrimary();
    var primaryDB = primary.getDB(dbName);
    assert(containsDropPendingCollection(primaryDB, collToDrop36),
           "Collection was removed on clean shutdown when FCV is 3.6.");

    // Let the secondary apply the collection drop operation, so that the replica set commit point
    // will advance, and the 'Commit' phase of the collection drop will complete on the primary.
    jsTestLog("Restarting oplog application on the secondary node.");
    var secondary = replTest.getSecondary();
    setFailPoint(secondary, "rsSyncApplyStop", "off");

    jsTestLog("Waiting for collection drop operation to replicate to all nodes.");
    replTest.awaitReplication();

    // Make sure the collection has been fully dropped. It should not appear as
    // a normal collection or under the 'system.drop' namespace any longer. Physical collection
    // drops may happen asynchronously, any time after the drop operation is committed, so we wait
    // to make sure the collection is eventually dropped.
    assert.soonNoExcept(function() {
        return !containsDropPendingCollection(primaryDB, collToDrop36);
    });

    replTest.stopSet();
}());
