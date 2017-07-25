/**
 * TwoPhaseDropCollectionTest is a library for testing two phase collection drops in a replica set.
 *
 * External tests can utilize this library to verify various aspects of the two phase collection
 * drop behavior. It provides a way to easily create a replicated collection and control its
 * transition between the 'PREPARE' and 'COMMIT' phase of a collection drop.
 *
 * Details:
 *
 * The test uses a 2 node replica set to verify both phases of a 2-phase collection drop: the
 * 'Prepare' and 'Commit' phase. Executing a 'drop' collection command should put that collection
 * into the 'Prepare' phase. The 'Commit' phase (physically dropping the collection) of a drop
 * operation with optime T should only be executed when C >= T, where C is the majority commit point
 * of the replica set.
 *
 */
let TwoPhaseDropCollectionTest = function(testName, dbName) {
    "use strict";

    load("jstests/libs/check_log.js");  // For 'checkLog'.

    let self = this;
    let oplogApplicationFailpoint = "rsSyncApplyStop";

    /**
     * Log a message for 'TwoPhaseDropCollectionTest'.
     */
    function _testLog(msg) {
        jsTestLog("[TwoPhaseDropCollectionTest] " + msg);
    }

    /**
     * Pause oplog application on a specified node.
     */
    self.pauseOplogApplication = function(node) {
        assert.commandWorked(
            node.adminCommand({configureFailPoint: oplogApplicationFailpoint, mode: "alwaysOn"}));
        checkLog.contains(node, oplogApplicationFailpoint + " fail point enabled");
    };

    /**
     * Resume oplog application on a specified node.
     */
    self.resumeOplogApplication = function(node) {
        assert.commandWorked(
            node.adminCommand({configureFailPoint: oplogApplicationFailpoint, mode: "off"}));
    };

    /**
     * Return a list of all collections in a given database. Use 'args' as the 'listCollections'
     * command arguments.
     */
    self.listCollections = function(database, args) {
        args = args || {};
        let failMsg = "'listCollections' command failed";
        let res = assert.commandWorked(database.runCommand("listCollections", args), failMsg);
        return res.cursor.firstBatch;
    };

    /**
     * Return a list of all collection names in a given database.
     */
    self.listCollectionNames = function(database, args) {
        return self.listCollections(database, args).map(c => c.name);
    };

    /**
     * Initiates a 2 node replica set to be used for the test. Returns the constructed ReplSetTest.
     */
    self.initReplSet = function() {
        let nodes = [{}, {rsConfig: {priority: 0}}];
        self.replTest = new ReplSetTest({name: testName, nodes: nodes});

        self.dbName = dbName;

        // Initiate the replica set.
        self.replTest.startSet();
        self.replTest.initiate();
        self.replTest.awaitReplication();

        return self.replTest;
    };

    /**
     * Creates a collection with name 'collName' in the test database and then awaits replication.
     */
    self.createCollection = function(collName) {
        // Create the collection that will be dropped and let it replicate.
        let primaryDB = self.replTest.getPrimary().getDB(self.dbName);
        assert.commandWorked(primaryDB.createCollection(collName));
        self.replTest.awaitReplication();
    };

    /**
     * Return a regex matching a drop-pending namespace string for a collection with name
     * 'collName'.
     *
     * Drop pending names should be of the format "system.drop.<optime>.<collectionName>", where
     * 'optime' is the optime of the collection drop operation, encoded as a string, and
     * 'collectionName' is the original collection name.
     */
    self.pendingDropRegex = function(collName) {
        return new RegExp("system\.drop\..*\." + collName + "$");
    };

    /**
     * Returns true if the collection 'collName' exists on the primary.
     */
    self.collectionExists = function(collName) {
        let primaryDB = self.replTest.getPrimary().getDB(self.dbName);
        let coll = self.listCollections(primaryDB).find(c => c.name === collName);
        return coll !== undefined;
    };

    /**
     * If 'collName' is in drop pending state on the primary, returns the name of the collection
     * after drop pending rename. If collection is not in drop pending state, returns false.
     */
    self.collectionIsPendingDrop = function(collName) {
        let primaryDB = self.replTest.getPrimary().getDB(self.dbName);
        let collections = self.listCollections(primaryDB, {includePendingDrops: true});

        _testLog("Checking presence of drop-pending collection for " + collName +
                 " in the collection list: " + tojson(collections));

        let pendingDropRegex = self.pendingDropRegex(collName);
        return collections.find(c => pendingDropRegex.test(c.name));
    };

    /**
     * Puts a collection with name 'collName' into the drop pending state. Returns the name of the
     * collection after it has been renamed to the 'system.drop' namespace.
     */
    self.prepareDropCollection = function(collName) {

        let primaryDB = self.replTest.getPrimary().getDB(self.dbName);

        // Pause application on secondary so that commit point doesn't advance, meaning that a
        // dropped collection on the primary will remain in 'drop-pending' state.
        _testLog("Pausing oplog application on the secondary node.");
        self.pauseOplogApplication(self.replTest.getSecondary());

        // Drop the collection on the primary.
        _testLog("Dropping collection '" + collName + "' on primary node.");
        assert.commandWorked(primaryDB.runCommand({drop: collName, writeConcern: {w: 1}}));

        // Make sure the collection doesn't appear in the normal collection list and that it is now
        // in 'drop-pending' state.
        assert(!self.collectionExists(collName));
        let droppedColl = self.collectionIsPendingDrop(collName);

        assert(
            droppedColl,
            "Dropped collection '" + collName + "' was not found in the 'system.drop' namespace");

        return droppedColl.name;
    };

    /**
     * Restarts oplog application on the secondary and waits for the drop of collection 'collName'
     * to be committed (physically dropped).
     */
    self.commitDropCollection = function(collName) {
        // Let the secondary apply the collection drop operation, so that the replica set commit
        // point will advance, and the 'Commit' phase of the collection drop will complete on the
        // primary.
        _testLog("Restarting oplog application on the secondary node.");
        self.resumeOplogApplication(self.replTest.getSecondary());

        _testLog("Waiting for collection drop operation to replicate to all nodes.");
        self.replTest.awaitReplication();

        // Make sure the collection has been fully dropped. It should not appear as a normal
        // collection or under the 'system.drop' namespace any longer. Physical collection drops may
        // happen asynchronously, any time after the drop operation is committed, so we wait to make
        // sure the collection is eventually dropped.
        _testLog("Waiting for collection drop of '" + collName + "' to commit.");
        assert.soonNoExcept(function() {
            assert(!self.collectionExists(collName));
            assert(!self.collectionIsPendingDrop(collName));
            return true;
        });
    };

    /**
     * Disable all fail points and shut down the replica set.
     */
    self.stop = function() {
        _testLog("Disabling fail points and shutting down replica set.");
        self.resumeOplogApplication(self.replTest.getSecondary());
        self.replTest.stopSet();
    };
};