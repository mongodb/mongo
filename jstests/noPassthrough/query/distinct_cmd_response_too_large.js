/**
 * Confirms that mongod will return an error when the result generated for a distinct command
 * exceeds MaxBSONSize.
 */
const conn = MongoRunner.runMongod();
const db = conn.getDB('test');
const coll = db.test;

const largeString = 'x'.repeat(1000 * 1000);

let bulk = coll.initializeUnorderedBulkOp();
for (let x = 0; x < 17; ++x) {
    bulk.insert({_id: (largeString + x.toString())});
}
assert.commandWorked(bulk.execute());

assert.commandFailedWithCode(db.runCommand({distinct: "test", key: "_id", query: {}}), 17217);

MongoRunner.stopMongod(conn);
