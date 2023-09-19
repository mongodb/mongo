/**
 * Verify that collections cannot be created with the WiredTiger 'encryption=()' option.
 *
 * @tags: [
 *  requires_wiredtiger
 * ]
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const collectionConfig = {
    storageEngine: {wiredTiger: {configString: "encryption=(keyid=key)"}}
};
const createResult = db.createCollection("coll", collectionConfig);
assert.commandFailedWithCode(createResult, ErrorCodes.IllegalOperation);

MongoRunner.stopMongod(conn);
