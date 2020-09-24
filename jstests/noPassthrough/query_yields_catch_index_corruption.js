// @tags: [requires_persistence, requires_journaling]
(function() {
"use strict";

const name = "query_yields_catch_index_corruption";
const dbpath = MongoRunner.dataPath + name + "/";

resetDbpath(dbpath);

let mongod = MongoRunner.runMongod({dbpath: dbpath});
assert.neq(null, mongod, "mongod failed to start.");

let db = mongod.getDB("test");

assert.commandWorked(db.adminCommand({
    configureFailPoint: "skipUnindexingDocumentWhenDeleted",
    mode: "alwaysOn",
    data: {indexName: "a_1_b_1"}
}));

let coll = db.getCollection(name);
coll.drop();

assert.commandWorked(coll.createIndex({a: 1, b: 1}));

// Corrupt the collection by inserting a document and then deleting it without deleting its index
// entry (thanks to the "skipUnindexingDocumentWhenDeleted" failpoint).
function createDanglingIndexEntry(doc) {
    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.remove(doc));

    // Validation should now fail.
    const validateRes = assert.commandWorked(coll.validate());
    assert.eq(false, validateRes.valid);

    // A query that accesses the now dangling index entry should fail with a
    // "DataCorruptionDetected" error.
    const error = assert.throws(() => coll.find(doc).toArray());
    assert.eq(error.code, ErrorCodes.DataCorruptionDetected, error);
}

createDanglingIndexEntry({a: 1, b: 1});

// Fix the index by rebuilding it, and ensure that it validates.
assert.commandWorked(coll.dropIndex({a: 1, b: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

let validateRes = assert.commandWorked(coll.validate());
assert.eq(true, validateRes.valid, tojson(validateRes));

// Reintroduce the dangling index entry, and this time fix it using the "repair" flag.
createDanglingIndexEntry({a: 1, b: 1});

MongoRunner.stopMongod(mongod, MongoRunner.EXIT_CLEAN, {skipValidation: true});
mongod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true, repair: ""});
assert.eq(null, mongod, "Expect this to exit cleanly");

// Verify that the server starts up successfully after the repair and that validate() now succeeds.
mongod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
assert.neq(null, mongod, "mongod failed to start after repair");

db = mongod.getDB("test");
coll = db.getCollection(name);

validateRes = assert.commandWorked(coll.validate());
assert.eq(true, validateRes.valid, tojson(validateRes));

MongoRunner.stopMongod(mongod);
})();
