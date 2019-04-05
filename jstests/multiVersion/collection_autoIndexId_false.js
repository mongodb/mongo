/*
 * If a collection does not have an index on the _id field (because of the use of autoIndexId: false
 * in 3.6), the latest version of MongoDB will build the missing index.
 *
 * Ensure that if an index cannot be built, MongoDB shuts down cleanly and allows for any duplicate
 * documents to be deleted in the previous version.
 */
(function() {
    'use strict';
    load('jstests/libs/get_index_helpers.js');

    const dbName = 'test';
    const collName = 'collection_autoIndexId_false';

    // Given a dbpath to a 3.6 server, attempt to upgrade to 4.0.
    function upgrade36To40(dbpath) {
        let conn = MongoRunner.runMongod({binVersion: '4.0', dbpath: dbpath, noCleanData: true});
        assert.neq(null, conn, 'mongod was unable to start with version 4.0');
        let adminDb = conn.getDB('admin');
        assert.commandWorked(adminDb.runCommand({'setFeatureCompatibilityVersion': '4.0'}));

        MongoRunner.stopMongod(conn);
    }

    // Given a dbpath to a 3.6 server, attempt to upgrade to latest.
    // shouldPass determines whether or not the upgrade should be successful.
    function upgrade40ToLatest(dbpath, shouldPass) {
        if (shouldPass) {
            let conn =
                MongoRunner.runMongod({binVersion: 'latest', dbpath: dbpath, noCleanData: true});
            assert.neq(null, conn, 'mongod failed to start with latest version');

            // Ensure the _id index exists.
            let testDb = conn.getDB(dbName);
            let coll = testDb.getCollection(collName);
            let spec = GetIndexHelpers.findByKeyPattern(coll.getIndexes(), {_id: 1});
            assert.neq(null, spec);

            MongoRunner.stopMongod(conn);
        } else {
            let conn = MongoRunner.runMongod(
                {binVersion: 'latest', dbpath: dbpath, noCleanData: true, waitForConnect: false});

            // This tests that the server shuts down cleanly despite the inability to build the _id
            // index.
            assert.eq(MongoRunner.EXIT_NEED_DOWNGRADE, waitProgram(conn.pid));
        }
    }

    // Create a collection with autoIndexId: false on a 3.6 server and assert that an upgrade to
    // latest fails because there are duplicate values for the _id index.
    function cannotUpgradeWithDuplicateIds() {
        let conn = MongoRunner.runMongod({binVersion: '3.6'});
        assert.neq(null, conn, 'mongod was unable able to start with version 3.6');

        const dbpath = conn.dbpath;

        // Create a collection with autoIndexId: false.
        let testDb = conn.getDB(dbName);
        let coll = testDb.getCollection(collName);
        assert.commandWorked(coll.runCommand('create', {autoIndexId: false}));
        assert.commandWorked(coll.insert({_id: 0, a: 1}));
        assert.commandWorked(coll.insert({_id: 0, a: 2}));
        MongoRunner.stopMongod(conn);

        // The upgrade to 4.0 should always succeed because it does not care about the _id index.
        upgrade36To40(dbpath);

        let shouldPass = false;
        upgrade40ToLatest(dbpath, shouldPass);

        // Remove the duplicate (now in 4.0) and retry the upgrade to 4.2.
        conn = MongoRunner.runMongod({binVersion: '4.0', dbpath: dbpath, noCleanData: true});
        testDb = conn.getDB(dbName);
        coll = testDb.getCollection(collName);
        assert.commandWorked(coll.remove({_id: 0, a: 2}));
        MongoRunner.stopMongod(conn);

        shouldPass = true;
        upgrade40ToLatest(dbpath, shouldPass);

        resetDbpath(dbpath);
    }

    // Create a collection with autoIndexId: false on a 3.6 server and assert that an upgrade to
    // latest succeeds because the missing _id index is built.
    function canUpgradeWithoutIndex() {
        let conn = MongoRunner.runMongod({binVersion: '3.6'});
        assert.neq(null, conn, 'mongod was unable able to start with version 3.6');

        const dbpath = conn.dbpath;

        // Create a collection with autoIndexId: false.
        let testDb = conn.getDB(dbName);
        let coll = testDb.getCollection(collName);
        assert.commandWorked(coll.runCommand('create', {autoIndexId: false}));
        assert.commandWorked(coll.insert({_id: 0, a: 1}));
        MongoRunner.stopMongod(conn);

        // The upgrade to 4.0 should always succeed because it does not care about the _id index.
        upgrade36To40(dbpath);

        const shouldPass = true;
        upgrade40ToLatest(dbpath, shouldPass);

        resetDbpath(dbpath);
    }

    cannotUpgradeWithDuplicateIds();
    canUpgradeWithoutIndex();
})();
