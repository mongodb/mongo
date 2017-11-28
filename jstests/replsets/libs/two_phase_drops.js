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
"use strict";

load("jstests/libs/check_log.js");  // For 'checkLog'.

class TwoPhaseDropCollectionTest {
    constructor(testName, dbName) {
        this.testName = testName;
        this.dbName = dbName;

        this.oplogApplicationFailpoint = "rsSyncApplyStop";
    }

    /**
     * Log a message for 'TwoPhaseDropCollectionTest'.
     */
    static _testLog(msg) {
        jsTestLog("[TwoPhaseDropCollectionTest] " + msg);
    }

    /**
     * Pause oplog application on a specified node.
     */
    pauseOplogApplication(node) {
        assert.commandWorked(node.adminCommand(
            {configureFailPoint: this.oplogApplicationFailpoint, mode: "alwaysOn"}));
        checkLog.contains(node, this.oplogApplicationFailpoint + " fail point enabled");
    }

    /**
     * Resume oplog application on a specified node.
     */
    resumeOplogApplication(node) {
        assert.commandWorked(
            node.adminCommand({configureFailPoint: this.oplogApplicationFailpoint, mode: "off"}));
    }

    /**
     * Return a list of all collections in a given database. Use 'args' as the 'listCollections'
     * command arguments.
     */
    static listCollections(database, args) {
        args = args || {};
        let failMsg = "'listCollections' command failed";
        let res = assert.commandWorked(database.runCommand("listCollections", args), failMsg);
        return res.cursor.firstBatch;
    }

    /**
     * Return a list of all collection names in a given database.
     */
    listCollectionNames(database, args) {
        return TwoPhaseDropCollectionTest.listCollections(database, args).map(c => c.name);
    }

    /**
     * Initiates a 2 node replica set to be used for the test. Returns the constructed ReplSetTest.
     */
    initReplSet() {
        let nodes = [{}, {rsConfig: {priority: 0}}];
        this.replTest = new ReplSetTest({name: this.testName, nodes: nodes});

        // Initiate the replica set.
        this.replTest.startSet();
        this.replTest.initiate();
        this.replTest.awaitReplication();

        return this.replTest;
    }

    /**
     * Creates a collection with name 'collName' in the test database and then awaits replication.
     */
    createCollection(collName) {
        // Create the collection that will be dropped and let it replicate.
        let primaryDB = this.replTest.getPrimary().getDB(this.dbName);
        assert.commandWorked(primaryDB.createCollection(collName));
        this.replTest.awaitReplication();
    }

    /**
     * Return a regex matching a drop-pending namespace string for a collection with name
     * 'collName'.
     *
     * Drop pending names should be of the format "system.drop.<optime>.<collectionName>", where
     * 'optime' is the optime of the collection drop operation, encoded as a string, and
     * 'collectionName' is the original collection name.
     */
    static pendingDropRegex(collName) {
        return new RegExp("system\.drop\..*\." + collName + "$");
    }

    /**
     * Returns true if the collection 'collName' exists on the primary.
     */
    collectionExists(collName) {
        let primaryDB = this.replTest.getPrimary().getDB(this.dbName);
        let coll =
            TwoPhaseDropCollectionTest.listCollections(primaryDB).find(c => c.name === collName);
        return coll !== undefined;
    }

    /**
     * If 'collName' is in drop pending state on the primary, returns the name of the collection
     * after drop pending rename. If collection is not in drop pending state, returns false.
     */
    collectionIsPendingDrop(collName) {
        let primaryDB = this.replTest.getPrimary().getDB(this.dbName);
        return TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(primaryDB, collName);
    }

    /**
     * If 'collName' in database 'db' is in drop pending state on the primary, returns the name
     * of the collection after drop pending rename. If collection is not in drop pending state,
     * returns false.
     */
    static collectionIsPendingDropInDatabase(db, collName) {
        let collections =
            TwoPhaseDropCollectionTest.listCollections(db, {includePendingDrops: true});

        TwoPhaseDropCollectionTest._testLog("Checking presence of drop-pending collection for " +
                                            collName + " in the collection list: " +
                                            tojson(collections));

        let pendingDropRegex = TwoPhaseDropCollectionTest.pendingDropRegex(collName);
        return collections.find(c => pendingDropRegex.test(c.name));
    }

    /**
     * Waits until 'collName' in database 'db' is not in drop pending state.
     */
    static waitForDropToComplete(db, collName) {
        assert.soon(
            () => !TwoPhaseDropCollectionTest.collectionIsPendingDropInDatabase(db, collName));
    }

    /**
     * Puts a collection with name 'collName' into the drop pending state. Returns the name of the
     * collection after it has been renamed to the 'system.drop' namespace.
     */
    prepareDropCollection(collName) {
        let primaryDB = this.replTest.getPrimary().getDB(this.dbName);

        // Pause application on secondary so that commit point doesn't advance, meaning that a
        // dropped collection on the primary will remain in 'drop-pending' state.
        TwoPhaseDropCollectionTest._testLog("Pausing oplog application on the secondary node.");
        this.pauseOplogApplication(this.replTest.getSecondary());

        // Drop the collection on the primary.
        TwoPhaseDropCollectionTest._testLog("Dropping collection '" + collName +
                                            "' on primary node.");
        assert.commandWorked(primaryDB.runCommand({drop: collName, writeConcern: {w: 1}}));

        // Make sure the collection doesn't appear in the normal collection list and that it is now
        // in 'drop-pending' state.
        assert(!this.collectionExists(collName));
        let droppedColl = this.collectionIsPendingDrop(collName);

        assert(
            droppedColl,
            "Dropped collection '" + collName + "' was not found in the 'system.drop' namespace");

        return droppedColl.name;
    }

    /**
     * Restarts oplog application on the secondary and waits for the drop of collection 'collName'
     * to be committed (physically dropped).
     */
    commitDropCollection(collName) {
        // Let the secondary apply the collection drop operation, so that the replica set commit
        // point will advance, and the 'Commit' phase of the collection drop will complete on the
        // primary.
        TwoPhaseDropCollectionTest._testLog("Restarting oplog application on the secondary node.");
        this.resumeOplogApplication(this.replTest.getSecondary());

        TwoPhaseDropCollectionTest._testLog(
            "Waiting for collection drop operation to replicate to all nodes.");
        this.replTest.awaitReplication();

        // Make sure the collection has been fully dropped. It should not appear as a normal
        // collection or under the 'system.drop' namespace any longer. Physical collection drops may
        // happen asynchronously, any time after the drop operation is committed, so we wait to make
        // sure the collection is eventually dropped.
        TwoPhaseDropCollectionTest._testLog("Waiting for collection drop of '" + collName +
                                            "' to commit.");
        // Bind the member functions onto this instead of the anonymous function.
        const twoPhaseDrop = this;
        assert.soonNoExcept(function() {
            assert(!twoPhaseDrop.collectionExists(collName));
            assert(!twoPhaseDrop.collectionIsPendingDrop(collName));
            return true;
        });
    }

    /**
     * Disable all fail points and shut down the replica set.
     */
    stop() {
        TwoPhaseDropCollectionTest._testLog("Disabling fail points and shutting down replica set.");
        this.resumeOplogApplication(this.replTest.getSecondary());
        this.replTest.stopSet();
    }
}
