// Cannot upgrade a server to latest if a collection does not have an index on the _id field.

// Create a collection with autoIndexId: false on a 3.6 server and ensure that an upgrade to latest
// fails.
(function() {
    'use strict';
    load('jstests/libs/get_index_helpers.js');

    // Given a connection to a 3.6 server, attempt to upgrade to 4.0 and then to latest.
    // shouldPass determines whether or not the upgrade should be successful.
    function upgrade36ToLatest(conn, shouldPass) {
        const dbpath = conn.dbpath;

        // Start with 4.0 because we can't skip more than one major version at a time.
        MongoRunner.stopMongod(conn);
        conn = MongoRunner.runMongod({binVersion: '4.0', dbpath: dbpath, noCleanData: true});
        assert.neq(null, conn, 'mongod was unable to start with version 4.0');
        let adminDb = conn.getDB('admin');
        assert.commandWorked(adminDb.runCommand({'setFeatureCompatibilityVersion': '4.0'}));

        // Start again with latest.
        MongoRunner.stopMongod(conn);
        conn = MongoRunner.runMongod({binVersion: 'latest', dbpath: dbpath, noCleanData: true});

        if (shouldPass) {
            assert.neq(null, conn, 'mongod failed to start with latest version');
            MongoRunner.stopMongod(conn);
        } else {
            assert.eq(null, conn, 'mongod started with latest version but should have failed');
        }

        resetDbpath(dbpath);
    }

    // Create a collection with autoIndexId: false on a 3.6 server and asset that an upgrade to
    // latest fails.
    function cannotUpgradeWithoutIndex() {
        let conn = MongoRunner.runMongod({binVersion: '3.6'});
        assert.neq(null, conn, 'mongod was unable able to start with version 3.6');

        // Create a collection with autoIndexId: false.
        let testDb = conn.getDB('test');
        let coll = testDb.coll;
        assert.commandWorked(coll.runCommand('create', {autoIndexId: false}));

        upgrade36ToLatest(conn, false);
    }

    // Create a collection with autoIndexId: false on a 3.6 server, manually create an index on the
    // _id field, and ensure that an upgrade to latest passes.
    function canUpgradeAfterCreatingIndex() {
        let conn = MongoRunner.runMongod({binVersion: '3.6'});
        assert.neq(null, conn, 'mongod was unable able to start with version 3.6');

        // Create a collection with autoIndexId: false and then manually create the _id index.
        let testDb = conn.getDB('test');
        let coll = testDb.coll;
        assert.commandWorked(coll.runCommand('create', {autoIndexId: false}));
        assert.commandWorked(coll.ensureIndex({_id: 1}, {name: '_id_'}));

        upgrade36ToLatest(conn, true);
    }

    cannotUpgradeWithoutIndex();
    canUpgradeAfterCreatingIndex();
})();
