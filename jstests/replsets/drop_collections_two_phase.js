/**
 * Test to ensure that two phase drop behavior for collections on replica sets works properly.
 *
 * Uses a 2 node replica set to verify both phases of a 2-phase collection drop: the 'Prepare' and
 * 'Commit' phase. Executing a 'drop' collection command should put that collection into the
 * 'Prepare' phase. The 'Commit' phase (physically dropping the collection) of a drop operation with
 * optime T should only be executed when C >= T, where C is the majority commit point of the replica
 * set.
 *
 * Verifies that collections get properly get moved into the "system.drop" namespace while pending
 * drop, and then physically dropped once the commit point advances. Also checks that the dbHash
 * does not include drop pending collections.
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

    // Compute db hash for all collections on given database.
    function getDbHash(database) {
        var res =
            assert.commandWorked(database.runCommand({dbhash: 1}), "'dbHash' command failed.");
        return res.md5;
    }

    // Set a fail point on a specified node.
    function setFailPoint(node, failpoint, mode) {
        assert.commandWorked(node.adminCommand({configureFailPoint: failpoint, mode: mode}));
    }

    var testName = "drop_collections_two_phase";
    var replTest = new ReplSetTest({name: testName, nodes: 2});

    // Initiate the replica set.
    replTest.startSet();
    replTest.initiate();
    replTest.awaitReplication();

    var primary = replTest.getPrimary();
    var secondary = replTest.getSecondary();

    var primaryDB = primary.getDB(testName);
    var collToDrop = "collectionToDrop";
    var collections, collection;

    // Create the collection that will be dropped and let it replicate.
    primaryDB.createCollection(collToDrop);
    replTest.awaitReplication();

    // Pause application on secondary so that commit point doesn't advance, meaning that a dropped
    // collection on the primary will remain in 'drop-pending' state.
    jsTestLog("Pausing oplog application on the secondary node.");
    setFailPoint(secondary, "rsSyncApplyStop", "alwaysOn");

    // Make sure the collection was created.
    var collNames = listCollectionNames(primaryDB);
    assert.contains(
        collToDrop, collNames, "Collection '" + collToDrop + "' wasn't created properly");

    /**
     * DROP COLLECTION PREPARE PHASE
     */

    // Drop the collection on the primary.
    jsTestLog("Dropping collection '" + collToDrop + "' on primary node.");
    assert.commandWorked(primaryDB.runCommand({drop: collToDrop}, {writeConcern: {w: 1}}));

    // Make sure the collection is now in 'drop-pending' state. The collection name should be of the
    // format "system.drop.<optime>.<collectionName>", where 'optime' is the optime of the
    // collection drop operation, encoded as a string, and 'collectionName' is the original
    // collection name.
    var pendingDropRegex = new RegExp("system\.drop\..*\." + collToDrop + "$");

    collections = listCollections(primaryDB, {includePendingDrops: true});
    collection = collections.find(c => pendingDropRegex.test(c.name));
    assert(collection,
           "Collection was not found in the 'system.drop' namespace. Full collection list: " +
               tojson(collections));

    // Make sure the collection doesn't appear in the normal collection list. Also check that
    // the default 'listCollections' behavior excludes drop-pending collections.
    collections = listCollections(primaryDB, {includePendingDrops: false});
    assert.eq(undefined, collections.find(c => c.name === collToDrop));

    collections = listCollections(primaryDB);
    assert.eq(undefined, collections.find(c => c.name === collToDrop));

    // Save the dbHash while drop is in 'pending' state.
    var dropPendingDbHash = getDbHash(primaryDB);

    /**
     * DROP COLLECTION COMMIT PHASE
     */

    // Let the secondary apply the collection drop operation, so that the replica set commit point
    // will advance, and the 'Commit' phase of the collection drop will complete on the primary.
    jsTestLog("Restarting oplog application on the secondary node.");
    setFailPoint(secondary, "rsSyncApplyStop", "off");

    jsTestLog("Waiting for collection drop operation to replicate to all nodes.");
    replTest.awaitReplication();

    // Make sure the collection has been fully dropped. It should not appear as
    // a normal collection or under the 'system.drop' namespace any longer. Physical collection
    // drops may happen asynchronously, any time after the drop operation is committed, so we wait
    // to make sure the collection is eventually dropped.
    assert.soonNoExcept(function() {
        var collections = listCollections(primaryDB, {includePendingDrops: true});
        assert.eq(undefined, collections.find(c => c.name === collToDrop));
        assert.eq(undefined, collections.find(c => pendingDropRegex.test(c.name)));
        return true;
    });

    // Save the dbHash after the drop has been committed.
    var dropCommittedDbHash = getDbHash(primaryDB);

    // The dbHash calculation should ignore drop pending collections. Therefore, therefore, the hash
    // during prepare phase and commit phase should match.
    var failMsg = "dbHash during drop pending phase did not match dbHash after drop was committed.";
    assert.eq(dropPendingDbHash, dropCommittedDbHash, failMsg);

    replTest.stopSet();

}());
