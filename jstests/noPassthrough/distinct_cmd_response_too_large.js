/**
 * Confirms that mongod will return an error when the result generated for a distinct command
 * exceeds MaxBSONSize.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();
const db = conn.getDB('test');
const coll = db.test;

const largeString = new Array(1000 * 1000).join('x');

let bulk = coll.initializeUnorderedBulkOp();
for (let x = 0; x < 17; ++x) {
    bulk.insert({_id: (largeString + x.toString())});
}
assert.commandWorked(bulk.execute());

assert.commandFailedWithCode(db.runCommand({distinct: "test", key: "_id", query: {}}), 17217);

MongoRunner.stopMongod(conn);
})();
