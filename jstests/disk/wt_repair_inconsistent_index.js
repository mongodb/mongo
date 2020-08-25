/**
 * Tests that --repair on WiredTiger correctly and gracefully handles inconsistent indexes.
 *
 * @tags: [requires_wiredtiger]
 */

(function() {

load('jstests/disk/libs/wt_file_helper.js');

const baseName = "wt_repair_inconsistent_index";
const collName = "test";
const dbpath = MongoRunner.dataPath + baseName + "/";

/**
 * Run the test by supplying additional parameters to MongoRunner.runMongod with 'mongodOptions'.
 */
let runTest = function(mongodOptions) {
    jsTestLog("Running test with args: " + tojson(mongodOptions));

    /**
     * Configure the skipIndexNewRecords failpoint, then insert documents into testColl, which will
     * result in an index inconsistency. Run repair and verify that the index is fixed by validate
     * without needing to be fully rebuilt.
     */
    (function testInconsistentIndex() {
        jsTestLog("Testing a repair on an inconsistent index");
        resetDbpath(dbpath);
        let mongod = startMongodOnExistingPath(dbpath, mongodOptions);
        let testColl = mongod.getDB(baseName)[collName];

        const doc = {a: 1};
        assert.commandWorked(testColl.insert(doc));

        const indexName = "a_1";
        assert.commandWorked(testColl.createIndex({a: 1}, {name: indexName}));
        assertQueryUsesIndex(testColl, doc, indexName);

        const testCollUri = getUriForColl(testColl);
        const indexUri = getUriForIndex(testColl, indexName);

        const db = mongod.getDB(baseName);
        assert.commandWorked(
            db.adminCommand({configureFailPoint: 'skipIndexNewRecords', mode: 'alwaysOn'}));
        assert.commandWorked(testColl.insert({a: 2}));

        // Disable validation because it is expected to not pass due to index inconsistencies.
        MongoRunner.stopMongod(mongod, null, {skipValidation: true});

        assertRepairSucceeds(dbpath, mongod.port, mongodOptions);
        mongod = startMongodOnExistingPath(dbpath, mongodOptions);
        testColl = mongod.getDB(baseName)[collName];

        // Repair doesn't create new idents because validate repair mode fixed index
        // inconsistencies.
        assert.eq(indexUri, getUriForIndex(testColl, indexName));

        assertQueryUsesIndex(testColl, doc, indexName);
        assert.eq(testCollUri, getUriForColl(testColl));
        assert.eq(testColl.count(), 2);

        MongoRunner.stopMongod(mongod);
    })();

    /**
     * Truncate an index and set the validate memory limit low enough so that validate repair mode
     * cannot fix the index. This causes the index to be entirely rebuilt.
     */
    (function testInconsistentIndexPartialRepair() {
        jsTestLog("Testing a partial repair on an inconsistent index");
        resetDbpath(dbpath);
        let mongod = startMongodOnExistingPath(dbpath, mongodOptions);
        let testColl = mongod.getDB(baseName)[collName];

        // Insert a document that is the size of the validate memory limit so that validate is
        // unable to report and fix all inconsistencies during startup repair.
        const bigDoc = {a: 'x'.repeat(1024 * 1024)};
        assert.commandWorked(testColl.insert(bigDoc));

        const smallDoc = {a: 1};
        assert.commandWorked(testColl.insert(smallDoc));

        const indexName = "a_1";
        assert.commandWorked(testColl.createIndex({a: 1}, {name: indexName}));
        assertQueryUsesIndex(testColl, {a: 1}, indexName);

        const testCollUri = getUriForColl(testColl);
        const indexUri = getUriForIndex(testColl, indexName);

        // Remove all index entries and restart.
        mongod = truncateUriAndRestartMongod(indexUri, mongod, mongodOptions);

        // Disable validation because it is expected to fail due to index inconsistencies.
        MongoRunner.stopMongod(mongod, null, {skipValidation: true});

        // Impose a memory limit so that only one index key can be detected and repaired.
        const options = mongodOptions;
        options["setParameter"] = "maxValidateMemoryUsageMB=1";
        assertRepairSucceeds(dbpath, mongod.port, options);
        mongod = startMongodOnExistingPath(dbpath, options);
        testColl = mongod.getDB(baseName)[collName];

        // Repair should create a new ident because validate repair is unable to fix all index
        // inconsistencies.
        assert.neq(indexUri, getUriForIndex(testColl, indexName));

        assertQueryUsesIndex(testColl, {a: 1}, indexName);
        assert.eq(testCollUri, getUriForColl(testColl));
        assert.eq(testColl.count(), 2);

        MongoRunner.stopMongod(mongod);
    })();
};

runTest({});
runTest({directoryperdb: ""});
runTest({wiredTigerDirectoryForIndexes: ""});
})();
