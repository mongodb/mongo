// Makes sure that TTL indexes on capped collection are allowed to be loaded if they already exist.
// This ensures that it is safe for old versions with such indexes already existing to upgrade to
// the latest version, even if such indexes are no longer allowed to be built on the latest version.
//
// @tags: [requires_persistence]
(function() {
"use strict";

load('jstests/libs/fail_point_util.js');

const dbName = "test";
const collName = jsTestName() + "_coll";

// Set up a TTL index on a capped collection, using a failpoint to bypass the check that should
// prevent this.
let conn = MongoRunner.runMongod();
const fp = configureFailPoint(conn, 'ignoreTTLIndexCappedCollectionCheck');
let db = conn.getDB(dbName);
let coll = db.getCollection(collName);

assert.commandWorked(db.createCollection(collName, {capped: true, size: 102400}));
assert.commandWorked(coll.createIndex({foo: 1}));
assert.commandWorked(coll.createIndex({bar: 1}, {expireAfterSeconds: 10}));
fp.off();
MongoRunner.stopMongod(conn);

// Check that the server can startup successfully with a pre-existing TTL index on a capped
// collection.
conn = MongoRunner.runMongod({
    restart: true,
    cleanData: false,
    dbpath: conn.dbpath,
});
db = conn.getDB(dbName);
coll = db.getCollection(collName);

// Since the failpoint is disabled, ensure a new TTL index cannot be created.
assert.commandFailedWithCode(coll.createIndex({baz: 1}, {expireAfterSeconds: 10}),
                             ErrorCodes.CannotCreateIndex);

// Ensure listIndexes returns the TTL index on the capped collection even though a new TTL index
// cannot be created.
const res = assert.commandWorked(db.runCommand({listIndexes: collName}));
const indexes = res.cursor.firstBatch;
assert.eq(
    3, indexes.length, "Unexpected number of indexes in listIndexes output: " + tojson(indexes));
assert.eq(indexes[2].expireAfterSeconds, 10, "Index is not TTL as expected: " + tojson(indexes));

MongoRunner.stopMongod(conn);
})();
