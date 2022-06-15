/**
 * If reIndex crashes after dropping indexes but before rebuilding them, a collection may exist
 * without an _id index. On startup, mongod should automatically build any missing _id indexes.
 *
 * @tags: [
 *   requires_persistence
 * ]
 */
(function() {

load("jstests/libs/index_catalog_helpers.js");  // For IndexCatalogHelpers.

// This test triggers an unclean shutdown, which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

const baseName = 'reindex_crash_rebuilds_id_index';
const collName = baseName;
const dbpath = MongoRunner.dataPath + baseName + '/';
resetDbpath(dbpath);

const mongodOptions = {
    dbpath: dbpath,
    noCleanData: true
};
let conn = MongoRunner.runMongod(mongodOptions);

let testDB = conn.getDB('test');
let testColl = testDB.getCollection(collName);

// Insert a single document and create the collection.
testColl.insert({a: 1});
let spec = IndexCatalogHelpers.findByKeyPattern(testColl.getIndexes(), {_id: 1});
assert.neq(null, spec, "_id index not found");
assert.eq("_id_", spec.name, tojson(spec));

// Enable a failpoint that causes reIndex to crash after dropping the indexes but before
// rebuilding them.
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'reIndexCrashAfterDrop', mode: 'alwaysOn'}));
assert.throws(() => testColl.runCommand({reIndex: collName}));

// The server should have crashed from the failpoint.
MongoRunner.stopMongod(conn, null, {allowedExitCode: MongoRunner.EXIT_ABRUPT});

// The server should start up successfully after rebuilding the _id index.
conn = MongoRunner.runMongod(mongodOptions);
testDB = conn.getDB('test');
testColl = testDB.getCollection(collName);
assert(testColl.exists());

// The _id index should exist.
spec = IndexCatalogHelpers.findByKeyPattern(testColl.getIndexes(), {_id: 1});
assert.neq(null, spec, "_id index not found");
assert.eq("_id_", spec.name, tojson(spec));

MongoRunner.stopMongod(conn);
})();
