/**
 * Verify that collections and index tables cannot be created with the WiredTiger 'type=lsm'
 * option.
 *
 * @tags: [
 *  requires_wiredtiger
 * ]
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());
const collName = "coll";
const collectionConfig = {
    storageEngine: {wiredTiger: {configString: "type=lsm"}}
};
const createResult = db.createCollection(collName, collectionConfig);
assert.commandFailedWithCode(createResult, 6627201);

const coll = db.getCollection(collName);
const createIndexResult =
    coll.createIndex({field: 1}, {storageEngine: {wiredTiger: {configString: "type=lsm"}}});
assert.commandFailedWithCode(createIndexResult, 6627201);
MongoRunner.stopMongod(conn);