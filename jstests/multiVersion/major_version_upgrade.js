/**
 * Tests upgrading a standalone node or replica set through several major versions.
 *
 * For each version downloaded by the multiversion setup:
 * - Start a node or replica set of that version, without clearing data files from the previous
 *   iteration.
 * - Create a new collection.
 * - Insert a document into the new collection.
 * - Create an index on the new collection.
 *
 * This test requires mmapv1 since 2.6 is tested. This can be removed when 2.6 is dropped.
 * @tags: [requires_mmapv1]
 */

(function() {
    'use strict';

    load('jstests/libs/get_index_helpers.js');
    load('jstests/multiVersion/libs/multi_rs.js');
    load('jstests/multiVersion/libs/verify_versions.js');

    // Setup the dbpath for this test.
    const dbpath = MongoRunner.dataPath + 'major_version_upgrade';
    resetDbpath(dbpath);

    // We set noCleanData to true in order to preserve the data files between iterations.
    const defaultOptions = {
        dbpath: dbpath,
        noCleanData: true,
    };

    // This lists all supported releases and needs to be kept up to date as versions are added and
    // dropped.
    // TODO SERVER-26792: In the future, we should have a common place from which both the
    // multiversion setup procedure and this test get information about supported major releases.
    const versions = [
        {binVersion: '2.6', testCollection: 'two_six'},
        {binVersion: '3.0', testCollection: 'three_zero'},
        {binVersion: '3.2', testCollection: 'three_two'},
        {binVersion: '3.2.1', testCollection: 'three_two_one'},
        {binVersion: 'last-stable', testCollection: 'last_stable'},
        {binVersion: 'latest', testCollection: 'latest'},
    ];

    // Standalone
    // Iterate from earliest to latest versions specified in the versions list, and follow the steps
    // outlined at the top of this test file.
    for (let i = 0; i < versions.length; i++) {
        let version = versions[i];
        let latestOptions = Object.extend({binVersion: version.binVersion}, defaultOptions);
        var changedAuthMechanism = false;
        if (TestData.authMechanism === "SCRAM-SHA-1" && version.binVersion === "2.6") {
            TestData.authMechanism = undefined;
            DB.prototype._defaultAuthenticationMechanism = "MONGODB-CR";
            changedAuthMechanism = true;
        }

        // Start a mongod with specified version.
        let conn = MongoRunner.runMongod(latestOptions);
        assert.neq(
            null, conn, 'mongod was unable to start up with options: ' + tojson(latestOptions));
        assert.binVersion(conn, version.binVersion);

        // Connect to the 'test' database.
        let testDB = conn.getDB('test');

        // Verify that the data and indices from previous iterations are still accessible.
        for (let j = 0; j < i; j++) {
            let oldVersionCollection = versions[j].testCollection;
            assert.eq(1,
                      testDB[oldVersionCollection].count(),
                      `data from ${oldVersionCollection} should be available; options: ` +
                          tojson(latestOptions));
            assert.neq(
                null,
                GetIndexHelpers.findByKeyPattern(testDB[oldVersionCollection].getIndexes(), {a: 1}),
                `index from ${oldVersionCollection} should be available; options: ` +
                    tojson(latestOptions));
        }

        // Create a new collection.
        assert.commandWorked(testDB.createCollection(version.testCollection));

        // Insert a document into the new collection.
        assert.writeOK(testDB[version.testCollection].insert({a: 1}));
        assert.eq(
            1,
            testDB[version.testCollection].count(),
            `mongo should have inserted 1 document into collection ${version.testCollection}; ` +
                'options: ' + tojson(latestOptions));

        // Create an index on the new collection.
        assert.commandWorked(testDB[version.testCollection].createIndex({a: 1}));

        // Shutdown the current mongod.
        MongoRunner.stopMongod(conn);

        if (version.binVersion === "2.6" && changedAuthMechanism) {
            TestData.authMechanism = "SCRAM-SHA-1";
            DB.prototype._defaultAuthenticationMechanism = "SCRAM-SHA-1";
            changedAuthMechanisms = false;
        }
    }

    // Replica Sets
    // Setup the ReplSetTest object.
    let nodes = {
        n1: {binVersion: versions[0].binVersion},
        n2: {binVersion: versions[0].binVersion},
        n3: {binVersion: versions[0].binVersion},
    };
    var changedAuthMechanisms = false;
    if (TestData.authMechanism === "SCRAM-SHA-1" && versions[0].binVersion === "2.6") {
        TestData.authMechanism = undefined;
        DB.prototype._defaultAuthenticationMechanism = "MONGODB-CR";
        changedAuthMechanism = true;
    }

    let rst = new ReplSetTest({nodes});

    // Start up and initiate the replica set.
    rst.startSet();

    // ReplSetTest.stepUp() requires replSetGetConfig, which is not available in 2.6, so we
    // use initiateWithAnyNodeAsPrimary() instead.
    rst.initiateWithAnyNodeAsPrimary();

    // Iterate from earliest to latest versions specified in the versions list, and follow the steps
    // outlined at the top of this test file.
    for (let i = 0; i < versions.length; i++) {
        let version = versions[i];
        rst.upgradeSet({binVersion: version.binVersion});
        if (version.binVersion != "2.6" && changedAuthMechanism) {
            TestData.authMechanism = "SCRAM-SHA-1";
            DB.prototype._defaultAuthenticationMechanism = "SCRAM-SHA-1";
            changedAuthMechanisms = false;
        }

        // Connect to the primary to ensure that the test can insert and create indices.
        let conn = rst.getPrimary();
        assert.neq(
            null, conn, `replica set was unable to start up with version: ${version.binVersion}`);
        assert.binVersion(conn, version.binVersion);

        // Connect to the 'test' database.
        let testDB = conn.getDB('test');

        // Verify that the data and indices from previous iterations are still accessible.
        for (let j = 0; j < i; j++) {
            let oldVersionCollection = versions[j].testCollection;
            assert.eq(
                1,
                testDB[oldVersionCollection].count(),
                `data from ${oldVersionCollection} should be available; nodes: ${tojson(nodes)}`);
            assert.neq(
                null,
                GetIndexHelpers.findByKeyPattern(testDB[oldVersionCollection].getIndexes(), {a: 1}),
                `index from ${oldVersionCollection} should be available; nodes: ${tojson(nodes)}`);
        }

        // Create a new collection.
        assert.commandWorked(testDB.createCollection(version.testCollection));

        // Insert a document into the new collection.
        assert.writeOK(testDB[version.testCollection].insert({a: 1}));
        assert.eq(
            1,
            testDB[version.testCollection].count(),
            `mongo should have inserted 1 document into collection ${version.testCollection}; ` +
                'nodes: ' + tojson(nodes));

        // Create an index on the new collection.
        assert.commandWorked(testDB[version.testCollection].createIndex({a: 1}));
    }

    // Stop the replica set.
    rst.stopSet();
})();
