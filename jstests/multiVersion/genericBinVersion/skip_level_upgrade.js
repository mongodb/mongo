/**
 * Tests that skip level upgrades from old binary versions to the latest binary version are both
 * not successful and do not corrupt anything to prevent correct binary version start up.
 *
 * For each binary version older than last-lts:
 * - Start a clean node of that version
 * - Create a new collection.
 * - Insert a document into the new collection.
 * - Create an index on the new collection.
 * - Benignly fail to upgrade to the latest version.
 * - Benignly fail to run --repair on the latest version.
 * - Successfully restart the node in the original version.
 * - Verify data files are intact.
 *
 * @tags: [requires_v4_0]
 */

import {IndexCatalogHelpers} from "jstests/libs/index_catalog_helpers.js";

const dbpath = MongoRunner.dataPath + "skip_level_upgrade";
resetDbpath(dbpath);

// We set noCleanData to true in order to preserve the data files within an iteration.
const defaultOptions = {
    dbpath: dbpath,
    noCleanData: true,
};

// This contains lists of LTS binary versions > 5.0 but < last-lts.
// TODO SERVER-76166: Generate this list programmatically.
//
// Note: To support PIT truncate functionality, the on-disk data format has been changed,
// which older versions (5.0 and below) cannot understand. This functionality is enabled
// for MongoDB from version 8.0 onwards. So, illegal skip-level upgrades from version 5.0 and older
// to version 8.0 or later would cause 8.0 (and later) to rewrite the older files to the newer
// format, which older versions don't understand. This requires manual intervention for the user to
// revert the database back to the older version. (See WT-12869 and WT-10307 for details). That's
// the reason this test will test from 6.0+.
const versions = [{binVersion: "6.0", testCollection: "six_zero"}];

// Iterate through versions specified in the versions list, and follow the steps outlined at
// the top of this test file.
for (let i = 0; i < versions.length; i++) {
    let version = versions[i];
    let mongodOptions = Object.extend({binVersion: version.binVersion}, defaultOptions);

    // Start up an old binary version mongod.
    let conn = MongoRunner.runMongod(mongodOptions);
    let port = conn.port;

    assert.neq(null, conn, "mongod was unable able to start with version " + tojson(version.binVersion));

    // Set up a collection on an old binary version node with one document and an index, and
    // then shut it down.
    let testDB = conn.getDB("test");
    assert.commandWorked(testDB.createCollection(version.testCollection));
    assert.commandWorked(testDB[version.testCollection].insert({a: 1}));
    assert.commandWorked(testDB[version.testCollection].createIndex({a: 1}));
    MongoRunner.stopMongod(conn);

    // Restart the mongod with the latest binary version on the old version's data files.
    // Should fail due to being a skip level upgrade.
    mongodOptions = Object.extend({binVersion: "latest"}, defaultOptions);
    assert.throws(() => MongoRunner.runMongod(mongodOptions));

    // Restart the mongod with the latest version with --repair. Should fail due to being a
    // skip level upgrade.
    let returnCode = runMongoProgram("mongod", "--port", port, "--repair", "--dbpath", dbpath);
    assert.neq(returnCode, 0, "expected mongod --repair to fail with a skip level upgrade");

    // Restart the mongod in the originally specified version. Should succeed.
    mongodOptions = Object.extend({binVersion: version.binVersion}, defaultOptions);
    conn = MongoRunner.runMongod(mongodOptions);

    // Verify that the data and indices from previous iterations are still accessible.
    testDB = conn.getDB("test");
    assert.eq(
        1,
        testDB[version.testCollection].count(),
        `data from ${version.testCollection} should be available; options: ` + tojson(mongodOptions),
    );
    assert.neq(
        null,
        IndexCatalogHelpers.findByKeyPattern(testDB[version.testCollection].getIndexes(), {a: 1}),
        `index from ${version.testCollection} should be available; options: ` + tojson(mongodOptions),
    );

    MongoRunner.stopMongod(conn);

    resetDbpath(dbpath);
}
