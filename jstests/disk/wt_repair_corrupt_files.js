/**
 * Tests that --repair on WiredTiger correctly and gracefully handles corrupt data files and
 * directories.
 *
 * @tags: [requires_wiredtiger]
 */

import {
    assertErrorOnStartupAfterIncompleteRepair,
    assertQueryUsesIndex,
    assertRepairFails,
    assertRepairSucceeds,
    corruptFile,
    getUriForColl,
    getUriForIndex,
    startMongodOnExistingPath,
} from "jstests/disk/libs/wt_file_helper.js";

const baseName = "wt_repair_corrupt_files";
const collName = "test";
const dbpath = MongoRunner.dataPath + baseName + "/";

/**
 * Run the test by supplying additional paramters to MongoRunner.runMongod with 'mongodOptions'.
 */
let runTest = function (mongodOptions) {
    resetDbpath(dbpath);
    jsTestLog("Running test with args: " + tojson(mongodOptions));

    /**
     * Test 1. Create a collection, corrupt its .wt file in an unrecoverable way, run repair.
     * Verify that repair succeeds at rebuilding it. An empty collection should be visible on
     * normal startup.
     */

    let mongod = startMongodOnExistingPath(dbpath, mongodOptions);
    let testColl = mongod.getDB(baseName)[collName];

    const doc = {a: 1};
    assert.commandWorked(testColl.insert(doc));

    let testCollUri = getUriForColl(testColl);
    let testCollFile = dbpath + testCollUri + ".wt";

    MongoRunner.stopMongod(mongod);

    jsTestLog("corrupting collection file: " + testCollFile);
    corruptFile(testCollFile);

    assertRepairSucceeds(dbpath, mongod.port, mongodOptions);

    mongod = startMongodOnExistingPath(dbpath, mongodOptions);
    testColl = mongod.getDB(baseName)[collName];

    assert.eq(testCollUri, getUriForColl(testColl));
    assert.eq(testColl.find({}).itcount(), 0);
    assert.eq(testColl.count(), 0);

    /**
     * Test 2. Corrupt an index file in an unrecoverable way. Verify that repair rebuilds and
     * allows MongoDB to start up normally.
     */

    assert.commandWorked(testColl.insert(doc));

    const indexName = "a_1";
    assert.commandWorked(testColl.createIndex({a: 1}, {name: indexName}));
    assertQueryUsesIndex(testColl, doc, indexName);

    let indexUri = getUriForIndex(testColl, indexName);

    MongoRunner.stopMongod(mongod);

    let indexFile = dbpath + indexUri + ".wt";
    jsTestLog("corrupting index file: " + indexFile);
    corruptFile(indexFile);

    assertRepairSucceeds(dbpath, mongod.port, mongodOptions);
    mongod = startMongodOnExistingPath(dbpath, mongodOptions);
    testColl = mongod.getDB(baseName)[collName];

    // Repair creates new idents.
    assert.neq(indexUri, getUriForIndex(testColl, indexName));

    assertQueryUsesIndex(testColl, doc, indexName);
    assert.eq(testColl.find(doc).itcount(), 1);
    assert.eq(testColl.count(), 1);

    MongoRunner.stopMongod(mongod);

    /**
     * Test 3. Corrupt the _mdb_catalog in an unrecoverable way. Verify that repair suceeds
     * in creating an empty catalog and recovers the orphaned testColl, which will still be
     * accessible in the 'local.orphan-' namespace.
     */

    let mdbCatalogFile = dbpath + "_mdb_catalog.wt";
    jsTestLog("corrupting catalog file: " + mdbCatalogFile);
    corruptFile(mdbCatalogFile);

    assertRepairSucceeds(dbpath, mongod.port, mongodOptions);

    mongod = startMongodOnExistingPath(dbpath, mongodOptions);
    testColl = mongod.getDB(baseName)[collName];
    assert.isnull(testColl.exists());
    assert.eq(testColl.find(doc).itcount(), 0);
    assert.eq(testColl.count(), 0);

    // Ensure the collection orphan was created with the existing document.
    const orphanCollName = "orphan." + testCollUri.replace(/-/g, "_");
    let orphanColl = mongod.getDB("local").getCollection(orphanCollName);
    assert(orphanColl.exists());
    assert.eq(orphanColl.find(doc).itcount(), 1);
    assert.eq(orphanColl.count(), 1);

    MongoRunner.stopMongod(mongod);

    /**
     * Test 4. Create two collections, and an index on each. Corrupt one collection's .wt file
     * in an unrecoverable way, leave the other as is, then run repair.
     * Verify that repair rebuilds the index on the corrupted collection but does not rebuild
     * the index on the unaffected collection.
     */

    let createIndexedColl = function (collName) {
        let coll = mongod.getDB(baseName)[collName];
        assert.commandWorked(coll.insert(doc));
        assert.commandWorked(coll.createIndex({a: 1}, {name: indexName}));
        assertQueryUsesIndex(coll, doc, indexName);
        return coll;
    };

    const corruptedCollName = "corrupted_coll";
    const healthyCollName = "healthy_coll";

    mongod = startMongodOnExistingPath(dbpath, mongodOptions);

    let corruptedColl = createIndexedColl(corruptedCollName);
    let corruptedOriginalIndexUri = getUriForIndex(corruptedColl, indexName);
    let corruptedCollUri = getUriForColl(corruptedColl);

    let healthyColl = createIndexedColl(healthyCollName);
    let healthyIndexUri = getUriForIndex(healthyColl, indexName);
    let healthyCollUri = getUriForColl(healthyColl);

    let corruptedCollFile = dbpath + corruptedCollUri + ".wt";

    MongoRunner.stopMongod(mongod);

    jsTestLog("corrupting collection file: " + corruptedCollFile);
    corruptFile(corruptedCollFile);

    assertRepairSucceeds(dbpath, mongod.port, mongodOptions);
    mongod = startMongodOnExistingPath(dbpath, mongodOptions);

    corruptedColl = mongod.getDB(baseName)[corruptedCollName];
    healthyColl = mongod.getDB(baseName)[healthyCollName];

    assert.neq(corruptedOriginalIndexUri, getUriForIndex(corruptedColl, indexName));
    assert.eq(healthyIndexUri, getUriForIndex(healthyColl, indexName));

    MongoRunner.stopMongod(mongod);

    /**
     * Test 5. Corrupt the WiredTigerHS file in an unrecoverable way. Run repair and verify that
     * MongoDB was rebuilt properly and starts up normally.
     */

    let corruptedWiredTigerHSFile = dbpath + "WiredTigerHS.wt";
    jsTestLog("corrupting WT History Store file: " + corruptedWiredTigerHSFile);
    corruptFile(corruptedWiredTigerHSFile);

    assertRepairFails(dbpath, mongod.port);
    assertErrorOnStartupAfterIncompleteRepair(dbpath, mongod.port);
};

runTest({});
runTest({directoryperdb: ""});
runTest({wiredTigerDirectoryForIndexes: ""});
