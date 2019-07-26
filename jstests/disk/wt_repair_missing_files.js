/**
 * Tests that --repair on WiredTiger correctly and gracefully handles missing data files and
 * directories.
 *
 * @tags: [requires_wiredtiger]
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');

const baseName = "wt_repair_missing_files";
const collName = "test";
const dbpath = MongoRunner.dataPath + baseName + "/";

resetDbpath(dbpath);

/**
 * Test 1. Create a collection, delete it's .wt file, run repair. Verify that repair succeeds at
 * re-creating it. The collection should be visible on normal startup.
 */

let mongod = startMongodOnExistingPath(dbpath);
let testColl = mongod.getDB(baseName)[collName];

const doc = {
    a: 1
};
assert.commandWorked(testColl.insert(doc));

let testCollUri = getUriForColl(testColl);
let testCollFile = dbpath + testCollUri + ".wt";

MongoRunner.stopMongod(mongod);

jsTestLog("deleting collection file: " + testCollFile);
removeFile(testCollFile);

assertRepairSucceeds(dbpath, mongod.port);

mongod = startMongodOnExistingPath(dbpath);
testColl = mongod.getDB(baseName)[collName];

assert.eq(testCollUri, getUriForColl(testColl));
assert.eq(testColl.find({}).itcount(), 0);
assert.eq(testColl.count(), 0);

/**
 * Test 2. Delete an index file. Verify that repair rebuilds and allows MongoDB to start up
 * normally.
 */

assert.commandWorked(testColl.insert(doc));

const indexName = "a_1";
assert.commandWorked(testColl.createIndex({a: 1}, {name: indexName}));
assertQueryUsesIndex(testColl, doc, indexName);

let indexUri = getUriForIndex(testColl, indexName);

MongoRunner.stopMongod(mongod);

let indexFile = dbpath + indexUri + ".wt";
jsTestLog("deleting index file: " + indexFile);
removeFile(indexFile);

assertRepairSucceeds(dbpath, mongod.port);
mongod = startMongodOnExistingPath(dbpath);
testColl = mongod.getDB(baseName)[collName];

// Repair creates new idents.
assert.neq(indexUri, getUriForIndex(testColl, indexName));

assertQueryUsesIndex(testColl, doc, indexName);
assert.eq(testColl.find(doc).itcount(), 1);
assert.eq(testColl.count(), 1);

MongoRunner.stopMongod(mongod);

/**
 * Test 3. Delete the sizeStorer. Verify that repair suceeds in recreating it.
 */

let sizeStorerFile = dbpath + "sizeStorer.wt";
jsTestLog("deleting size storer file: " + sizeStorerFile);
removeFile(sizeStorerFile);

assertRepairSucceeds(dbpath, mongod.port);

mongod = startMongodOnExistingPath(dbpath);
testColl = mongod.getDB(baseName)[collName];

assert.eq(testColl.find(doc).itcount(), 1);
assert.eq(testColl.count(), 1);
MongoRunner.stopMongod(mongod);

/**
 * Test 4. Delete the _mdb_catalog. Verify that repair suceeds in creating an empty catalog and
 * MongoDB starts up normally with no data.
 */

let mdbCatalogFile = dbpath + "_mdb_catalog.wt";
jsTestLog("deleting catalog file: " + mdbCatalogFile);
removeFile(mdbCatalogFile);

assertRepairSucceeds(dbpath, mongod.port);

mongod = startMongodOnExistingPath(dbpath);
testColl = mongod.getDB(baseName)[collName];
assert.isnull(testColl.exists());

assert.eq(testColl.find(doc).itcount(), 0);
assert.eq(testColl.count(), 0);

/**
 * Test 5. Verify that using repair with --directoryperdb creates a missing directory and its
 * files, allowing MongoDB to start up normally.
 */

MongoRunner.stopMongod(mongod);
resetDbpath(dbpath);

mongod = startMongodOnExistingPath(dbpath, {directoryperdb: ""});
testColl = mongod.getDB(baseName)[collName];

assert.commandWorked(testColl.insert(doc));

testCollUri = getUriForColl(testColl);

MongoRunner.stopMongod(mongod);

let dataDir = dbpath + baseName;
jsTestLog("deleting data directory: " + dataDir);
removeFile(dataDir);

assertRepairSucceeds(dbpath, mongod.port, {directoryperdb: ""});

mongod = startMongodOnExistingPath(dbpath, {directoryperdb: ""});
testColl = mongod.getDB(baseName)[collName];

assert.eq(testCollUri, getUriForColl(testColl));
assert.eq(testColl.find({}).itcount(), 0);
assert.eq(testColl.count(), 0);

MongoRunner.stopMongod(mongod);
})();
