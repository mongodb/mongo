/**
 * Verify that collections cannot be created with the WiredTiger 'type=lsm' option.
 *
 * @tags: [
 *  requires_wiredtiger
 * ]
 */

(function() {
'use strict';

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const collectionConfig = {
    storageEngine: {wiredTiger: {configString: "type=lsm"}}
};
const createResult = db.createCollection("coll", collectionConfig);
assert.commandFailedWithCode(createResult, 6627201);

MongoRunner.stopMongod(conn);
})();
