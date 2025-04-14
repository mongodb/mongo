/**
 * Wrapper around ReplSetTest to test dbCheck with old format unique index keys
 */

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

export class DbCheckOldFormatKeysTest {
    constructor({
        name = "DbCheckOldFormatKeysTest",
    }) {
        const rst = new ReplSetTest({name, nodes: 3});
        rst.startSet();
        rst.initiate();
        this._rst = rst;
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

        let fps = [];

        for (let n of this.getRst().nodes) {
            fps.push(configureFailPoint(n, "WTIndexCreateUniqueIndexesInOldFormat"));
            fps.push(configureFailPoint(n, "WTIndexInsertUniqueKeysInOldFormat"));
        }

        for (const indexSpec of indexSpecs) {
            assert.commandWorked(db.getCollection(collName).createIndex(indexSpec, {unique: true}));
        }

        jsTestLog(`Inserting documents into collection ${dbName}.${collName}`);
        const res = assert.commandWorked(
            db.runCommand({insert: collName, documents: data, writeConcern: {w: 'majority'}}));
        this.getRst().awaitReplication();
        assert.eq(db.getCollection(collName).find({}).count(), data.length);
        jsTestLog(`Inserted with w: majority, opTime ${tojson(res.operationTime)}`);

        for (let fp of fps) {
            fp.off();
        }

        forEachNonArbiterNode(this.getRst(), function(node) {
            assert.commandWorked(node.adminCommand({
                setParameter: 1,
                logComponentVerbosity: {command: 3},
                dbCheckHealthLogEveryNBatches: 1
            }));
        });
    }

    /**
     * Inserts data into the replica set across all nodes. To insert old format unique index keys,
     * this function must be called in v4.2 or earlier, prior to upgrading.
     */
    insertOldFormatKeyStrings(dbName,
                              collName,
                              indexSpecs = [{a: 1}, {b: -1}],
                              data = loadDummyData()) {
        this.insertIndexAndData(dbName, collName, indexSpecs, data);
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
            // that haven't set the failpoints will simply delete all documents.

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

    stop() {
        this._rst.stopSet();
    }
}
