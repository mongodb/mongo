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
    {binVersion: '3.6', featureCompatibilityVersion: '3.6', testCollection: 'three_six'},
    {binVersion: '4.0', featureCompatibilityVersion: '4.0', testCollection: 'four_zero'},
    {binVersion: '4.2', featureCompatibilityVersion: '4.2', testCollection: 'four_two'},
    {binVersion: '4.4', featureCompatibilityVersion: '4.4', testCollection: 'four_four'},
    {binVersion: 'last-lts', testCollection: 'last_lts'},
    {binVersion: 'last-continuous', testCollection: 'last_continuous'},
    {binVersion: 'latest', featureCompatibilityVersion: latestFCV, testCollection: 'latest'},
];

// Standalone
// Iterate from earliest to latest versions specified in the versions list, and follow the steps
// outlined at the top of this test file.
let authSchemaUpgraded = false;
for (let i = 0; i < versions.length; i++) {
    let version = versions[i];
    let mongodOptions = Object.extend({binVersion: version.binVersion}, defaultOptions);

    // Start a mongod with specified version.
    let conn = MongoRunner.runMongod(mongodOptions);

    if ((conn === null) && (i > 0) && !authSchemaUpgraded) {
        // As of 4.0, mongod will refuse to start up with authSchema 3
        // until the schema has been upgraded.
        // Step back a version (to 3.6) in order to perform the upgrade,
        // Then try startuing 4.0 again.
        print(
            "Failed starting mongod, going to try upgrading the auth schema on the prior version");
        conn = MongoRunner.runMongod(
            Object.extend({binVersion: versions[i - 1].binVersion}, defaultOptions));
        assert.neq(null,
                   conn,
                   'mongod was previously able to start with version ' +
                       tojson(version.binVersion) + " but now can't");
        assert.commandWorked(conn.getDB('admin').runCommand({authSchemaUpgrade: 1}));
        MongoRunner.stopMongod(conn);

        authSchemaUpgraded = true;
        conn = MongoRunner.runMongod(mongodOptions);
    }

    assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(mongodOptions));
    assert.binVersion(conn, version.binVersion);

    if ((i === 0) && (version.binVersion <= 3.6)) {
        // Simulate coming from a <= 2.6 installation where MONGODB-CR was the default/only
        // authentication mechanism. Eventually, the upgrade process will fail (above) when
        // running on 4.0 where support for MONGODB-CR has been removed.
        conn.getDB('admin').system.version.save({"_id": "authSchema", "currentVersion": 3});
    }

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
    assert.commandWorked(testDB[version.testCollection].insert({a: 1}));
    assert.eq(1,
              testDB[version.testCollection].count(),
              `mongo should have inserted 1 document into collection ${version.testCollection}; ` +
                  'options: ' + tojson(mongodOptions));

    // Create an index on the new collection.
    assert.commandWorked(testDB[version.testCollection].createIndex({a: 1}));

    // Set the appropriate featureCompatibilityVersion upon upgrade, if applicable.
    if (version.hasOwnProperty('featureCompatibilityVersion')) {
        let adminDB = conn.getDB("admin");
        assert.commandWorked(adminDB.runCommand(
            {"setFeatureCompatibilityVersion": version.featureCompatibilityVersion}));
    }

    // Shutdown the current mongod.
    MongoRunner.stopMongod(conn);
}

// Replica Sets
// Setup the ReplSetTest object.
let nodes = {
    n1: {binVersion: versions[0].binVersion},
    n2: {binVersion: versions[0].binVersion},
    n3: {binVersion: versions[0].binVersion},
};
let rst = new ReplSetTest({nodes});

// Start up and initiate the replica set.
rst.startSet();
rst.initiate();

// Iterate from earliest to latest versions specified in the versions list, and follow the steps
// outlined at the top of this test file.
for (let i = 0; i < versions.length; i++) {
    let version = versions[i];

    // Connect to the primary running the old version to ensure that the test can insert and
    // create indices.
    let primary = rst.getPrimary();

    // Upgrade the secondary nodes first.
    rst.upgradeSecondaries({binVersion: version.binVersion});

    assert.eq(
        primary, rst.getPrimary(), "Primary changed unexpectedly after upgrading secondaries");
    assert.neq(null,
               primary,
               `replica set was unable to start up after upgrading secondaries to version: ${
                   version.binVersion}`);

    // Connect to the 'test' database.
    let testDB = primary.getDB('test');
    assert.commandWorked(testDB.createCollection(version.testCollection));
    assert.commandWorked(testDB[version.testCollection].insert({a: 1}));
    assert.eq(1,
              testDB[version.testCollection].count(),
              `mongo should have inserted 1 document into collection ${version.testCollection}; ` +
                  'nodes: ' + tojson(nodes));

    // Create an index on the new collection.
    assert.commandWorked(testDB[version.testCollection].createIndex({a: 1}));

    // Do the index creation and insertion again after upgrading the primary node.
    primary = rst.upgradePrimary(primary, {binVersion: version.binVersion});
    assert.neq(
        null, primary, `replica set was unable to start up with version: ${version.binVersion}`);
    assert.binVersion(primary, version.binVersion);
    testDB = primary.getDB('test');

    assert.commandWorked(testDB[version.testCollection].insert({b: 1}));
    assert.eq(2,
              testDB[version.testCollection].count(),
              `mongo should have inserted 2 documents into collection ${version.testCollection}; ` +
                  'nodes: ' + tojson(nodes));

    assert.commandWorked(testDB[version.testCollection].createIndex({b: 1}));

    // Verify that all previously inserted data and indices are accessible.
    for (let j = 0; j <= i; j++) {
        let oldVersionCollection = versions[j].testCollection;
        assert.eq(2,
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

    // Set the appropriate featureCompatibilityVersion upon upgrade, if applicable.
    if (version.hasOwnProperty('featureCompatibilityVersion')) {
        let primaryAdminDB = primary.getDB("admin");
        assert.commandWorked(primaryAdminDB.runCommand(
            {setFeatureCompatibilityVersion: version.featureCompatibilityVersion}));
        rst.awaitReplication();
    }
}

// Stop the replica set.
rst.stopSet();
})();
