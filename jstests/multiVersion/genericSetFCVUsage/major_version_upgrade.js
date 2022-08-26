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
 * @tags: [requires_v4_0]
 */

(function() {
'use strict';

load('jstests/libs/index_catalog_helpers.js');
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
const versions = [
    {binVersion: '4.0', featureCompatibilityVersion: '4.0', testCollection: 'four_zero'},
    {binVersion: '4.2', featureCompatibilityVersion: '4.2', testCollection: 'four_two'},
    {binVersion: '4.4', featureCompatibilityVersion: '4.4', testCollection: 'four_four'},
    {binVersion: '5.0', featureCompatibilityVersion: '5.0', testCollection: 'five_zero'},
    {binVersion: '6.0', featureCompatibilityVersion: '6.0', testCollection: 'six_zero'},
    {binVersion: 'last-lts', testCollection: 'last_lts'},
    {binVersion: 'last-continuous', testCollection: 'last_continuous'},
    {binVersion: 'latest', featureCompatibilityVersion: latestFCV, testCollection: 'latest'},
];

// Standalone
// Iterate from earliest to latest versions specified in the versions list, and follow the steps
// outlined at the top of this test file.
for (let i = 0; i < versions.length; i++) {
    let version = versions[i];
    let mongodOptions = Object.extend({binVersion: version.binVersion}, defaultOptions);

    // Start a mongod with specified version.
    let conn = null;
    try {
        conn = MongoRunner.runMongod(mongodOptions);
    } catch (e) {
        print(e);
    }

    assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(mongodOptions));
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
            IndexCatalogHelpers.findByKeyPattern(testDB[oldVersionCollection].getIndexes(), {a: 1}),
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
            IndexCatalogHelpers.findByKeyPattern(testDB[oldVersionCollection].getIndexes(), {a: 1}),
            `index from ${oldVersionCollection} should be available; nodes: ${tojson(nodes)}`);
        assert.neq(
            null,
            IndexCatalogHelpers.findByKeyPattern(testDB[oldVersionCollection].getIndexes(), {b: 1}),
            `index from ${oldVersionCollection} should be available; nodes: ${tojson(nodes)}`);
    }

    // Set the appropriate featureCompatibilityVersion upon upgrade, if applicable.
    if (version.hasOwnProperty('featureCompatibilityVersion')) {
        let primaryAdminDB = primary.getDB("admin");
        assert.commandWorked(primaryAdminDB.runCommand(
            {setFeatureCompatibilityVersion: version.featureCompatibilityVersion}));
        rst.awaitReplication();
        // Make sure we reach the new featureCompatibilityVersion in the committed snapshot on
        // on all nodes before continuing to upgrade.
        // checkFCV does not work for version 3.4 (and below)
        if (version.featureCompatibilityVersion != '3.4') {
            for (let n of rst.nodes) {
                checkFCV(n.getDB("admin"), version.featureCompatibilityVersion);
            }
        }
    }
}

// Stop the replica set.
rst.stopSet();
})();
