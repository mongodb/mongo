/**
 * Tests upgrading a standalone node or replica set through several major versions.
 *
 * For each version downloaded by the multiversion setup:
 * - Start a node or replica set of that version, without clearing data files from the previous
 *   iteration.
 * - Create a new collection.
 * - Insert a document into the new collection.
 * - Create an index on the new collection.
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
        {binVersion: '3.0', testCollection: 'three_zero'},
        {binVersion: '3.2', testCollection: 'three_two'},
        {binVersion: '3.4', testCollection: 'three_four'},
        {binVersion: 'last-stable', testCollection: 'last_stable'},
        {binVersion: 'latest', testCollection: 'latest'},
    ];

    // These key patterns are considered valid for existing v:0 and v:1 indexes, but are considered
    // invalid for v:2 indexes or new index builds.
    var invalidIndexSpecs = [
        {a: 0},
        {a: NaN},
        {a: true},
    ];

    // When running the oldest supported version, insert indexes with bad key patterns.
    function insertBadIndexes(testDB) {
        invalidIndexSpecs.forEach((spec) => {
            // Generate a unique and identifiable collection name.
            let collName = 'bad_index_' + tojson(spec.a);
            assert.commandWorked(testDB[collName].createIndex(spec, {name: 'badkp'}),
                                 'failed to create index with key pattern' + tojson(spec));

        });
    }

    // When running the newest version, check that the indexes with bad key patterns are readable.
    function validateBadIndexesStandalone(testDB) {
        invalidIndexSpecs.forEach((spec) => {
            // Generate a unique and identifiable collection name.
            let collName = 'bad_index_' + tojson(spec.a);
            let indexSpec = GetIndexHelpers.findByName(testDB[collName].getIndexes(), 'badkp');
            assert.neq(null, indexSpec, 'could not find index "badkp"');
            assert.eq(1, indexSpec.v, tojson(indexSpec));

            // Collection compact command should succeed, despite the presence of the v:1 index
            // which would fail v:2 validation rules.
            assert.commandWorked(testDB.runCommand({compact: collName}));

            // repairDatabase should similarly succeed.
            assert.commandWorked(testDB.runCommand({repairDatabase: 1}));

            // reIndex should succeed.
            assert.commandWorked(testDB[collName].reIndex());

            // A query that hints the index should succeed.
            assert.commandWorked(testDB.runCommand({find: collName, hint: "badkp"}));

            // Newly created indexes will do stricter validation and should fail if the
            // key pattern is invalid.
            assert.commandWorked(testDB[collName].dropIndexes());
            assert.commandFailedWithCode(
                testDB[collName].createIndex(spec),
                ErrorCodes.CannotCreateIndex,
                'creating index with key pattern ' + tojson(spec) + ' unexpectedly succeeded');
            // Index build should also fail if v:1 or v:2 is explicitly requested.
            assert.commandFailedWithCode(
                testDB[collName].createIndex(spec, {v: 1}),
                ErrorCodes.CannotCreateIndex,
                'creating index with key pattern ' + tojson(spec) + ' unexpectedly succeeded');
            assert.commandFailedWithCode(
                testDB[collName].createIndex(spec, {v: 2}),
                ErrorCodes.CannotCreateIndex,
                'creating index with key pattern ' + tojson(spec) + ' unexpectedly succeeded');

        });
    }

    // Check that secondary nodes have the v:1 indexes.
    function validateBadIndexesSecondary(testDB) {
        invalidIndexSpecs.forEach((spec) => {
            // Generate a unique and identifiable collection name.
            let collName = 'bad_index_' + tojson(spec.a);
            // Verify that the secondary has the v:1 index.
            let indexSpec = GetIndexHelpers.findByName(testDB[collName].getIndexes(), 'badkp');
            assert.neq(null, indexSpec, 'could not find index "badkp"');
            assert.eq(1, indexSpec.v, tojson(indexSpec));
        });
    }

    // Standalone
    // Iterate from earliest to latest versions specified in the versions list, and follow the steps
    // outlined at the top of this test file.
    for (let i = 0; i < versions.length; i++) {
        let version = versions[i];
        let mongodOptions = Object.extend({binVersion: version.binVersion}, defaultOptions);
        var changedAuthMechanism = false;
        if (TestData.authMechanism === "SCRAM-SHA-1" && version.binVersion === "2.6") {
            TestData.authMechanism = undefined;
            DB.prototype._defaultAuthenticationMechanism = "MONGODB-CR";
            changedAuthMechanism = true;
        }

        // Start a mongod with specified version.
        let conn = MongoRunner.runMongod(mongodOptions);
        assert.neq(
            null, conn, 'mongod was unable to start up with options: ' + tojson(mongodOptions));
        assert.binVersion(conn, version.binVersion);

        // Connect to the 'test' database.
        let testDB = conn.getDB('test');

        // Verify that the data and indices from previous iterations are still accessible.
        for (let j = 0; j < i; j++) {
            let oldVersionCollection = versions[j].testCollection;
            assert.eq(1,
                      testDB[oldVersionCollection].count(),
                      `data from ${oldVersionCollection} should be available; options: ` +
                          tojson(mongodOptions));
            assert.neq(
                null,
                GetIndexHelpers.findByKeyPattern(testDB[oldVersionCollection].getIndexes(), {a: 1}),
                `index from ${oldVersionCollection} should be available; options: ` +
                    tojson(mongodOptions));
        }

        // Create a new collection.
        assert.commandWorked(testDB.createCollection(version.testCollection));

        // Insert a document into the new collection.
        assert.writeOK(testDB[version.testCollection].insert({a: 1}));
        assert.eq(
            1,
            testDB[version.testCollection].count(),
            `mongo should have inserted 1 document into collection ${version.testCollection}; ` +
                'options: ' + tojson(mongodOptions));

        // Create an index on the new collection.
        assert.commandWorked(testDB[version.testCollection].createIndex({a: 1}));

        if (i === 0) {
            // We're on the earliest version, insert indexes with bad key patterns.
            insertBadIndexes(testDB);
        } else if (i === versions.length - 1) {
            // We're on the latest version, check bad indexes are still readable.
            validateBadIndexesStandalone(testDB);
        }
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
    rst.initiate();

    // Iterate from earliest to latest versions specified in the versions list, and follow the steps
    // outlined at the top of this test file.
    for (let i = 0; i < versions.length; i++) {
        let version = versions[i];
        if (version.binVersion != "2.6" && changedAuthMechanism) {
            TestData.authMechanism = "SCRAM-SHA-1";
            DB.prototype._defaultAuthenticationMechanism = "SCRAM-SHA-1";
            changedAuthMechanisms = false;
        }

        // Connect to the primary running the old version to ensure that the test can insert and
        // create indices.
        let primary = rst.getPrimary();

        // Upgrade the secondary nodes first.
        rst.upgradeSecondaries(primary, {binVersion: version.binVersion});

        assert.neq(
            null,
            primary,
            `replica set was unable to start up after upgrading secondaries to version: ${version.binVersion}`);

        // Connect to the 'test' database.
        let testDB = primary.getDB('test');
        assert.commandWorked(testDB.createCollection(version.testCollection));
        assert.writeOK(testDB[version.testCollection].insert({a: 1}));
        assert.eq(
            1,
            testDB[version.testCollection].count(),
            `mongo should have inserted 1 document into collection ${version.testCollection}; ` +
                'nodes: ' + tojson(nodes));

        // Create an index on the new collection.
        assert.commandWorked(testDB[version.testCollection].createIndex({a: 1}));

        if (i === 0) {
            // We're on the earliest version, insert indexes with bad key patterns.
            insertBadIndexes(testDB);
        } else if (i === versions.length - 1) {
            // We're on the latest version, check bad indexes are still readable.
            for (let secondary of rst.getSecondaries()) {
                validateBadIndexesSecondary(secondary.getDB('test'));
            }
        }

        // Do the index creation and insertion again after upgrading the primary node.
        primary = rst.upgradePrimary(primary, {binVersion: version.binVersion});
        assert.neq(null,
                   primary,
                   `replica set was unable to start up with version: ${version.binVersion}`);
        assert.binVersion(primary, version.binVersion);
        testDB = primary.getDB('test');

        assert.writeOK(testDB[version.testCollection].insert({b: 1}));
        assert.eq(
            2,
            testDB[version.testCollection].count(),
            `mongo should have inserted 2 documents into collection ${version.testCollection}; ` +
                'nodes: ' + tojson(nodes));

        assert.commandWorked(testDB[version.testCollection].createIndex({b: 1}));

        // Verify that all previously inserted data and indices are accessible.
        for (let j = 0; j <= i; j++) {
            let oldVersionCollection = versions[j].testCollection;
            assert.eq(
                2,
                testDB[oldVersionCollection].count(),
                `data from ${oldVersionCollection} should be available; nodes: ${tojson(nodes)}`);
            assert.neq(
                null,
                GetIndexHelpers.findByKeyPattern(testDB[oldVersionCollection].getIndexes(), {a: 1}),
                `index from ${oldVersionCollection} should be available; nodes: ${tojson(nodes)}`);
            assert.neq(
                null,
                GetIndexHelpers.findByKeyPattern(testDB[oldVersionCollection].getIndexes(), {b: 1}),
                `index from ${oldVersionCollection} should be available; nodes: ${tojson(nodes)}`);
        }
    }

    // Stop the replica set.
    rst.stopSet();
})();
