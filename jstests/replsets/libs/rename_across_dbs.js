/**
 * Sets up a test for renaming a collection across different databases in a replica set.
 * 'options' format:
 * {
 *     nodes: <list of binary versions for nodes in replica set. optional>,
 *     setFeatureCompatibilityVersion: <FCV. optional>,
 *     dropTarget: <if true, creates target collection that will be dropped. Default: false>,
 * }
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {reconfig} from "jstests/replsets/rslib.js";

export var RenameAcrossDatabasesTest = function (options) {
    if (!(this instanceof RenameAcrossDatabasesTest)) {
        return new RenameAcrossDatabasesTest(options);
    }

    // Capture the 'this' reference
    let self = this;

    self.options = options || {};

    /**
     * Log a message for 'RenameAcrossDatabasesTest'.
     */
    function _testLog(msg) {
        jsTestLog("[RenameAcrossDatabasesTest] " + msg);
    }

    /**
     * Runs the test.
     */
    this.run = function () {
        const options = this.options;
        let nodes = [{}, {}, {}];
        if (options.nodes) {
            assert.eq(nodes.length, options.nodes.length);
            for (let i = 0; i < options.nodes.length; ++i) {
                nodes[i] = Object.merge(nodes[i], options.nodes[i]);
            }
        }
        _testLog("replica set node options: " + tojson(nodes));

        const replTest = new ReplSetTest({nodes: [nodes[0]], oplogSize: 1000});
        const testName = replTest.name;
        replTest.startSet();
        replTest.initiate();

        // This test performs a reconfig that will change the implicit default write concern
        // from {w: "majority"} to {w: 1}. In order for this reconfig to succeed, we must first
        // set the cluster-wide write concern.
        assert.commandWorked(
            replTest.getPrimary().adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: "majority"},
                writeConcern: {w: "majority"},
            }),
        );

        // If provided in 'options', we set the featureCompatibilityVersion. We do this prior to
        // adding any other members to the replica set.
        if (options.setFeatureCompatibilityVersion) {
            assert.commandWorked(
                replTest.getPrimary().adminCommand({
                    setFeatureCompatibilityVersion: options.setFeatureCompatibilityVersion,
                    confirm: true,
                }),
            );
        }

        for (let i = 1; i < nodes.length; ++i) {
            replTest.add(nodes[i]);
        }
        replTest.waitForAllNewlyAddedRemovals();

        const conns = replTest.nodes;
        const hosts = replTest.nodeList();
        const currentConfig = replTest.getReplSetConfigFromNode();
        const nextVersion = currentConfig.version + 1;
        const replSetConfig = {
            _id: currentConfig._id,
            protocolVersion: 1,
            members: [
                {
                    _id: 0,
                    host: hosts[0],
                },
                {
                    _id: 1,
                    host: hosts[1],
                    priority: 0,
                },
                {
                    _id: 2,
                    host: hosts[2],
                    arbiterOnly: true,
                },
            ],
            version: nextVersion,
        };

        reconfig(replTest, replSetConfig);

        replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
        replTest.awaitReplication();

        const primary = replTest.getPrimary();
        _testLog(
            "Feature compatibility version: " +
                assert.commandWorked(primary.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}))
                    .featureCompatibilityVersion,
        );

        // Rename a collection across databases that also drops the existing target collection.
        const sourceColl = primary.getCollection("sourceDb.from");
        const targetColl = primary.getCollection("targetDb.to");

        // Create target collection that will be dropped during the rename operation if
        // options.dropTarget is true.
        const dropTarget = options.dropTarget || false;
        if (dropTarget) {
            assert.commandWorked(targetColl.insert({_id: 1000, target: 1}));
            assert.commandWorked(targetColl.createIndex({target: 1}));
        }

        // Populate the source collections and create indexes. Do the same for the target collection
        // if we are testing dropTarget.
        // Indexes are explicitly recreated during cross-database renames. We verify this by
        // checking the number of indexes in the target collection after the rename.
        const numDocs = 10;
        _testLog("Inserting " + numDocs + " documents into source collection.");
        for (let i = 0; i < numDocs; ++i) {
            assert.commandWorked(sourceColl.insert({_id: i, source: 1}));
        }
        const numNonIdIndexes = 3;
        _testLog("Creating " + numNonIdIndexes + " indexes.");
        for (let i = 0; i < numNonIdIndexes; ++i) {
            let keys = {};
            keys["x" + i] = 1;
            assert.commandWorked(sourceColl.createIndex(Object.merge(keys, {"source": 1})));
        }
        replTest.awaitReplication();

        _testLog(
            "Collections and indexes created. About to rename source collection " +
                sourceColl.getFullName() +
                " to " +
                targetColl.getFullName() +
                " with dropTarget set to " +
                dropTarget,
        );
        let results = assert.commandWorked(
            primary.adminCommand({
                renameCollection: sourceColl.getFullName(),
                to: targetColl.getFullName(),
                dropTarget: dropTarget,
            }),
        );

        assert.eq(
            0,
            sourceColl
                .getDB()
                .getCollectionInfos()
                .filter((coll) => coll.name === sourceColl.getFullName()).length,
        );
        assert.eq(numDocs, targetColl.find().itcount());
        assert.eq(numNonIdIndexes + 1, targetColl.getIndexes().length);
        _testLog("Rename across databases successful.");

        // Dump oplog entries. Do this before validating the server data because the verification
        // logic inserts additional oplog entries into the oplog collection.
        const numOplogEntriesToDump = 100;
        _testLog("Dumping last " + numOplogEntriesToDump + " oplog entries.");
        replTest.dumpOplog(primary, {op: {$ne: "n"}}, numOplogEntriesToDump);

        // Make sure oplogs & dbHashes match
        _testLog("Checking oplogs and dbhashes after renaming collection.");
        replTest.awaitReplication();
        replTest.checkOplogs(testName);
        replTest.checkPreImageCollection(testName);
        replTest.checkReplicatedDataHashes(testName);

        _testLog("Test completed. Stopping replica set.");
        replTest.stopSet();
    };
};
