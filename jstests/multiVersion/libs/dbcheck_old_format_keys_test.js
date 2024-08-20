/**
 * Wrapper around ReplSetTest to test dbCheck with old format unique index keys
 */
import "jstests/multiVersion/libs/multi_rs.js";
import "jstests/multiVersion/libs/verify_versions.js";

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {forEachNonArbiterNode} from "jstests/replsets/libs/dbcheck_utils.js";

export const defaultNumDocs = 20;
export const defaultNumKeysInserted = 2 * defaultNumDocs;

function loadDummyData() {
    const testData = [];
    for (let i = 0; i < defaultNumDocs; ++i) {
        testData.push({_id: i, a: i, b: i});
    }
    return testData;
}

// TODO (SERVER-66611): Automatically modify this list.
const upgradeVersions = {
    "3.6": {"fcv": "4.0", "nextVersion": "4.0"},
    "4.0": {"fcv": "4.2", "nextVersion": "4.2"},
    "4.2": {"fcv": "4.4", "nextVersion": "4.4"},
    "4.4": {"fcv": "5.0", "nextVersion": "5.0"},
    "5.0": {"fcv": "6.0", "nextVersion": "6.0"},
    "6.0": {"fcv": "7.0", "nextVersion": "7.0"},
    "7.0": {"fcv": "8.0", "nextVersion": "8.0"},
    "8.0": {"fcv": "8.1", "nextVersion": "latest"},
    // TODO (SERVER-66611): Automate modifying this list.
    "latest": {}
};

export class DbCheckOldFormatKeysTest {
    constructor({
        name = "DbCheckOldFormatKeysTest",
        binVersion = "4.2",
        initiateWithHighElectionTimeout = true,
    }) {
        assert(binVersion in upgradeVersions);
        function performSetUp() {
            const nodes = {
                n1: {binVersion},
                n2: {binVersion},
                n3: {binVersion},
            };
            const rst = new ReplSetTest({name, nodes});
            rst.startSet();
            if (initiateWithHighElectionTimeout) {
                rst.initiateWithHighElectionTimeout();
            } else {
                rst.initiate();
            }
            return rst;
        }

        this._rst = performSetUp();
        this._binVersion = binVersion;
    }

    getRst() {
        return this._rst;
    }

    getPrimary() {
        return this._rst.getPrimary();
    }

    getSecondaries() {
        return this._rst.getSecondaries();
    }

    insertIndexAndData(dbName, collName, indexSpecs = [{a: 1}, {b: -1}], data = loadDummyData()) {
        jsTestLog("Creating indexes on: " + tojson(indexSpecs));
        const primary = this.getRst().getPrimary();
        const db = primary.getDB(dbName);
        for (const indexSpec of indexSpecs) {
            assert.commandWorked(db.getCollection(collName).createIndex(indexSpec, {unique: true}));
        }

        jsTestLog(`Inserting documents into collection ${dbName}.${collName}`);
        const res = assert.commandWorked(
            db.runCommand({insert: collName, documents: data, writeConcern: {w: 'majority'}}));
        this.getRst().awaitReplication();
        assert.eq(db.getCollection(collName).find({}).count(), data.length);
        jsTestLog(`Inserted with w: majority, opTime ${tojson(res.operationTime)}`);
    }

    /**
     * Inserts data into the replica set across all nodes. To insert old format unique index keys,
     * this function must be called in v4.2 or earlier, prior to upgrading.
     */
    insertOldFormatKeyStrings(dbName,
                              collName,
                              indexSpecs = [{a: 1}, {b: -1}],
                              data = loadDummyData()) {
        const primary = this.getPrimary();
        if (this._binVersion === "4.2") {
            // Downgrade FCV down to 4.0 so that insertion into a unique index uses the old
            // keystring format.
            assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
            this._rst.awaitReplication();
        }

        this.insertIndexAndData(dbName, collName, indexSpecs, data);

        if (this._binVersion === "4.2") {
            // Reupgrade FCV to 4.2 if it was downgraded.
            assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "4.2"}));
            this.getRst().awaitReplication();
            this.getRst().awaitLastOpCommitted();
        }
    }

    /**
     * Creates missing index keys by deleting index keys but skipping the deletion of the records.
     * This function should only be called after upgrading to latest and must be called on
     * pre-inserted data.
     */
    createMissingKeys(nodes, dbName, collName, deleteFilter = {}) {
        jsTestLog("Creating missing index keys");
        const fps = [];
        for (const node of nodes) {
            // Skip deleting records and the _id index when running a deletion commmand.
            const skipDeleteRecordFp = configureFailPoint(node, "skipDeleteRecord");
            const skipUnindexingDocWhenDeletedFp =
                configureFailPoint(node, "skipUnindexingDocumentWhenDeleted", {indexName: "_id_"});
            fps.push(skipDeleteRecordFp);
            fps.push(skipUnindexingDocWhenDeletedFp);
        }

        // Call delete on the docs. Deletes all documents if no filter is specified. Any nodes that
        // haven't set the failpoints will simply delete all documents.
        const coll = this.getPrimary().getDB(dbName).getCollection(collName);
        assert.commandWorked(coll.deleteMany(deleteFilter));
        this.getRst().awaitReplication();

        // Turn off all of the failpoints and return.
        for (const fp of fps) {
            fp.off();
        }
    }

    createMissingKeysOnPrimary(dbName, collName, deleteFilter = {}) {
        this.createMissingKeys([this.getPrimary()], dbName, collName, deleteFilter);
    }

    createMissingKeysOnSecondaries(dbName, collName, deleteFilter = {}) {
        this.createMissingKeys(this.getSecondaries(), dbName, collName, deleteFilter);
    }

    createMissingKeysOnAllNodes(dbName, collName, deleteFilter = {}) {
        this.createMissingKeys(this.getRst().nodes, dbName, collName, deleteFilter);
    }

    /**
     * Creates extra index keys by deleting records but skipping deleting/updating index keys.
     * This function should only be called after upgrading to latest and must be called on
     * pre-inserted data.
     */
    createExtraKeys(nodes, dbName, collName, failpointName, docFilter = {}) {
        jsTestLog("Creating extra index keys");
        const fps = [];
        for (const node of nodes) {
            const extraKeysFp = configureFailPoint(node, failpointName, {indexName: "a_1"});
            fps.push(extraKeysFp);
        }
        const coll = this.getPrimary().getDB(dbName).getCollection(collName);

        if (failpointName == "skipUnindexingDocumentWhenDeleted") {
            // Call delete on the docs. Deletes all documents if no filter is specified. Any nodes
            // that
            // haven't set the failpoints will simply delete all documents.

            assert.commandWorked(coll.deleteMany(docFilter));

        } else if (failpointName == "skipUpdatingIndexDocument") {
            assert.commandWorked(coll.updateMany(docFilter, {$unset: {"a": ""}}));
        }

        this.getRst().awaitReplication();

        // Turn off all of the failpoints and return.
        for (const fp of fps) {
            fp.off();
        }
    }

    createExtraKeysRecordNotFound(nodes, dbName, collName, docFilter = {}) {
        this.createExtraKeys(
            nodes, dbName, collName, "skipUnindexingDocumentWhenDeleted", docFilter);
    }

    createExtraKeysRecordDoesNotMatch(nodes, dbName, collName, docFilter = {}) {
        this.createExtraKeys(nodes, dbName, collName, "skipUpdatingIndexDocument", docFilter);
    }

    createExtraKeysRecordDoesNotMatchOnPrimary(dbName, collName, docFilter = {}) {
        this.createExtraKeysRecordDoesNotMatch([this.getPrimary()], dbName, collName, docFilter);
    }

    createExtraKeysRecordDoesNotMatchOnAllNodes(dbName, collName, docFilter = {}) {
        this.createExtraKeysRecordDoesNotMatch(this.getRst().nodes, dbName, collName, docFilter);
    }

    createExtraKeysRecordNotFoundOnSecondary(dbName, collName, docFilter = {}) {
        this.createExtraKeysRecordNotFound(this.getSecondaries(), dbName, collName, docFilter);
    }

    createExtraKeysRecordNotFoundOnAllNodes(dbName, collName, docFilter = {}) {
        this.createExtraKeysRecordNotFound(this.getRst().nodes, dbName, collName, docFilter);
    }

    /**
     * Upgrades the replica set all the way to latest.
     */
    upgradeRst() {
        assert(this._binVersion in upgradeVersions);
        while (this._binVersion != "latest") {
            const {fcv, nextVersion} = upgradeVersions[this._binVersion];
            jsTestLog("Upgrading to version: " + nextVersion);

            const rst = this.getRst();
            rst.upgradeSet({binVersion: nextVersion});

            if (fcv) {
                jsTestLog("Upgrading fcv: " + fcv);
                const primary = rst.getPrimary();
                const res = primary.adminCommand({"setFeatureCompatibilityVersion": fcv});
                if (!res.ok && res.code === 7369100) {
                    // We failed due to requiring 'confirm: true' on the command. This will only
                    // occur on 7.0+ nodes that have 'enableTestCommands' set to false. Retry the
                    // setFCV command with 'confirm: true'.
                    assert.commandWorked(primary.adminCommand(
                        {"setFeatureCompatibilityVersion": fcv, confirm: true}));
                } else {
                    assert.commandWorked(res);
                }
                rst.awaitReplication();
                // We need to ensure that the FCV is committed to the oplog before we shut down.
                // Otherwise the nodes may start up post-upgrade and check FCV before reconstructing
                // the journal, which will raise an invalid version error because they will see the
                // old FCV.
                rst.awaitLastOpCommitted();

                this._fcv = fcv;
            }
            this._binVersion = nextVersion;
        }
        // Verify that we have upgraded to latest.
        assert.eq("latest", this._binVersion);
        const fcvDoc = this.getRst().getPrimary().adminCommand(
            {getParameter: 1, featureCompatibilityVersion: 1});
        assert.eq(this._fcv, fcvDoc.featureCompatibilityVersion.version);

        forEachNonArbiterNode(this.getRst(), function(node) {
            assert.commandWorked(node.adminCommand({
                setParameter: 1,
                logComponentVerbosity: {command: 3},
                dbCheckHealthLogEveryNBatches: 1
            }));
        });
    }

    stop() {
        this._rst.stopSet();
    }
}
