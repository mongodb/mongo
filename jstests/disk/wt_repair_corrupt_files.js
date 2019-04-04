/**
 * Tests that --repair on WiredTiger correctly and gracefully handles corrupt data files and
 * directories.
 *
 * @tags: [requires_wiredtiger]
 */

(function() {

    load('jstests/disk/libs/wt_file_helper.js');

    const baseName = "wt_repair_corrupt_files";
    const collName = "test";
    const dbpath = MerizoRunner.dataPath + baseName + "/";

    /**
     * Run the test by supplying additional paramters to MerizoRunner.runMerizod with 'merizodOptions'.
     */
    let runTest = function(merizodOptions) {
        resetDbpath(dbpath);
        jsTestLog("Running test with args: " + tojson(merizodOptions));

        /**
         * Test 1. Create a collection, corrupt its .wt file in an unrecoverable way, run repair.
         * Verify that repair succeeds at rebuilding it. An empty collection should be visible on
         * normal startup.
         */

        let merizod = startMerizodOnExistingPath(dbpath, merizodOptions);
        let testColl = merizod.getDB(baseName)[collName];

        const doc = {a: 1};
        assert.commandWorked(testColl.insert(doc));

        let testCollUri = getUriForColl(testColl);
        let testCollFile = dbpath + testCollUri + ".wt";

        MerizoRunner.stopMerizod(merizod);

        jsTestLog("corrupting collection file: " + testCollFile);
        corruptFile(testCollFile);

        assertRepairSucceeds(dbpath, merizod.port, merizodOptions);

        merizod = startMerizodOnExistingPath(dbpath, merizodOptions);
        testColl = merizod.getDB(baseName)[collName];

        assert.eq(testCollUri, getUriForColl(testColl));
        assert.eq(testColl.find({}).itcount(), 0);
        assert.eq(testColl.count(), 0);

        /**
         * Test 2. Corrupt an index file in an unrecoverable way. Verify that repair rebuilds and
         * allows MerizoDB to start up normally.
         */

        assert.commandWorked(testColl.insert(doc));

        const indexName = "a_1";
        assert.commandWorked(testColl.createIndex({a: 1}, {name: indexName}));
        assertQueryUsesIndex(testColl, doc, indexName);

        let indexUri = getUriForIndex(testColl, indexName);

        MerizoRunner.stopMerizod(merizod);

        let indexFile = dbpath + indexUri + ".wt";
        jsTestLog("corrupting index file: " + indexFile);
        corruptFile(indexFile);

        assertRepairSucceeds(dbpath, merizod.port, merizodOptions);
        merizod = startMerizodOnExistingPath(dbpath, merizodOptions);
        testColl = merizod.getDB(baseName)[collName];

        // Repair creates new idents.
        assert.neq(indexUri, getUriForIndex(testColl, indexName));

        assertQueryUsesIndex(testColl, doc, indexName);
        assert.eq(testColl.find(doc).itcount(), 1);
        assert.eq(testColl.count(), 1);

        MerizoRunner.stopMerizod(merizod);

        /**
         * Test 3. Corrupt the _mdb_catalog in an unrecoverable way. Verify that repair suceeds
         * in creating an empty catalog and recovers the orphaned testColl, which will still be
         * accessible in the 'local.orphan-' namespace.
         */

        let mdbCatalogFile = dbpath + "_mdb_catalog.wt";
        jsTestLog("corrupting catalog file: " + mdbCatalogFile);
        corruptFile(mdbCatalogFile);

        assertRepairSucceeds(dbpath, merizod.port, merizodOptions);

        merizod = startMerizodOnExistingPath(dbpath, merizodOptions);
        testColl = merizod.getDB(baseName)[collName];
        assert.isnull(testColl.exists());
        assert.eq(testColl.find(doc).itcount(), 0);
        assert.eq(testColl.count(), 0);

        // Ensure the collection orphan was created with the existing document.
        const orphanCollName = "orphan." + testCollUri.replace(/-/g, "_");
        let orphanColl = merizod.getDB('local').getCollection(orphanCollName);
        assert(orphanColl.exists());
        assert.eq(orphanColl.find(doc).itcount(), 1);
        assert.eq(orphanColl.count(), 1);

        MerizoRunner.stopMerizod(merizod);
    };

    runTest({});
    runTest({directoryperdb: ""});
    runTest({wiredTigerDirectoryForIndexes: ""});
})();
