/**
 * Tests that mongod can startup in standalone mode with missing data files for user collections.
 * Performing any operations against collections with missing data files will result in a crash.
 *
 * @tags: [requires_wiredtiger]
 */
(function() {

load('jstests/disk/libs/wt_file_helper.js');

// This test triggers an unclean shutdown (an fassert), which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

const baseName = "wt_startup_with_missing_user_collection";
const dbpath = MongoRunner.dataPath + baseName + "/";

resetDbpath(dbpath);
let mongod = startMongodOnExistingPath(dbpath);

const dbName = "test";
let testDB = mongod.getDB(dbName);

assert.commandWorked(testDB.createCollection("a"));
assert.commandWorked(testDB.createCollection("b"));

assert.commandWorked(testDB.adminCommand({fsync: 1}));

const collUri = getUriForColl(testDB.getCollection("a"));
const indexUri = getUriForIndex(testDB.getCollection("a"), /*indexName=*/"_id_");

MongoRunner.stopMongod(mongod);

// Remove data files for collection "a" after shutting down.
removeFile(dbpath + "/" + collUri + ".wt");
removeFile(dbpath + "/" + indexUri + ".wt");

// Perform a startup and shutdown with no other operations in between.
mongod = startMongodOnExistingPath(dbpath);
assert.neq(null, mongod, "Failed to start");

// Disable validation because it is expected to fail when data files are missing.
MongoRunner.stopMongod(mongod, null, {skipValidation: true});

// Perform a startup and try to use collection "a". Mongod will crash.
mongod = startMongodOnExistingPath(dbpath);
assert.neq(null, mongod, "Failed to start");

testDB = mongod.getDB(dbName);

assert.throws(() => {
    assert.commandWorked(testDB.getCollection("a").insert({}));
});
assert.gte(rawMongoProgramOutput().search("Fatal assertion.*50883"), 0);

// Perform a startup, drop collection "a" and shutdown.
mongod = startMongodOnExistingPath(dbpath);
assert.neq(null, mongod, "Failed to start");

testDB = mongod.getDB(dbName);
assert(testDB.getCollection("a").drop());

MongoRunner.stopMongod(mongod);
}());
