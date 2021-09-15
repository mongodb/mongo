/**
 * Verifies that it is possible to upgrade a replica set with collections with 'recordPreImages'
 * option to use 'changeStreamPreAndPostImages' option, and to do a corresponding downgrade.
 * @tags: [requires_fcv_51, featureFlagChangeStreamPreAndPostImages]
 */
(function() {
'use strict';

load("jstests/libs/collection_drop_recreate.js");  // For assertCreateCollection.
load("jstests/libs/collection_options.js");        // For assertCollectionOptionIsEnabled,
                                                   // assertCollectionOptionIsAbsent.
load("jstests/multiVersion/libs/multi_rs.js");     // For upgradeSet.

const collName = "test";

function runTest(downgradeVersion) {
    const downgradeFCV = binVersionToFCV(downgradeVersion);

    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {binVersion: downgradeVersion},
    });
    rst.startSet();
    rst.initiate();

    // Create the collection with recorded pre-images enabled.
    let testDB = rst.getPrimary().getDB(jsTestName());
    assertCreateCollection(testDB, collName, {"recordPreImages": true});

    // Upgrade the replica set.
    rst.upgradeSet({binVersion: "latest"});
    testDB = rst.getPrimary().getDB(jsTestName());

    // Set the FCV to the latest.
    testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV});

    // 'changeStreamPreAndPostImages' field must be absent and 'recordPreImages' field must be set
    // to true.
    assertCollectionOptionIsEnabled(testDB, collName, "recordPreImages");
    assertCollectionOptionIsAbsent(testDB, collName, "changeStreamPreAndPostImages");

    // Enable pre-/post-images for the collection with "recordPreImages" enabled.
    assert.commandWorked(
        testDB.runCommand({"collMod": collName, "changeStreamPreAndPostImages": true}));

    // 'changeStreamPreAndPostImages' field must be set to true and 'recordPreImages' should be
    // absent.
    assertCollectionOptionIsAbsent(testDB, collName, "recordPreImages");
    assertCollectionOptionIsEnabled(testDB, collName, "changeStreamPreAndPostImages");

    // Set 'recordPreImages: true' to disable 'changeStreamPreAndPostImages' option.
    assert.commandWorked(testDB.runCommand({"collMod": collName, "recordPreImages": true}));

    // 'changeStreamPreAndPostImages' field must be absent and 'recordPreImages' should be set to
    // true.
    assertCollectionOptionIsEnabled(testDB, collName, "recordPreImages");
    assertCollectionOptionIsAbsent(testDB, collName, "changeStreamPreAndPostImages");

    // Downgrade the FCV.
    testDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV});

    // Verify that an attempt to set 'changeStreamPreAndPostImages' options fails for the downgrade
    // version.
    assert.commandFailedWithCode(
        testDB.createCollection(collName, {"changeStreamPreAndPostImages": false}), 5846900);
    assert.commandFailedWithCode(
        testDB.runCommand({"collMod": collName, "changeStreamPreAndPostImages": false}), 5846901);

    // Downgrade the cluster.
    rst.upgradeSet({binVersion: downgradeVersion});

    // Reset the db reference.
    testDB = rst.getPrimary().getDB(jsTestName());

    // 'changeStreamPreAndPostImages' field must be absent and 'recordPreImages' should be set to
    // true.
    assertCollectionOptionIsEnabled(testDB, collName, "recordPreImages");
    assertCollectionOptionIsAbsent(testDB, collName, "changeStreamPreAndPostImages");

    // Upgrade the replica set.
    rst.upgradeSet({binVersion: "latest"});
    testDB = rst.getPrimary().getDB(jsTestName());

    // Set the FCV to the latest.
    testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV});

    // Enable pre-/post-images for the collection with "recordPreImages" enabled.
    assert.commandWorked(
        testDB.runCommand({"collMod": collName, "changeStreamPreAndPostImages": true}));

    // 'changeStreamPreAndPostImages' field must be set to true and 'recordPreImages' field must be
    // absent.
    assertCollectionOptionIsEnabled(testDB, collName, "changeStreamPreAndPostImages");
    assertCollectionOptionIsAbsent(testDB, collName, "recordPreImages");

    // Downgrade the FCV.
    testDB.adminCommand({setFeatureCompatibilityVersion: downgradeFCV});

    // Downgrading the cluster should fail, since unsupported field 'changeStreamPreAndPostImages'
    // is set to true for the collection.
    try {
        rst.upgradeSet({binVersion: downgradeVersion});
        assert(false);
    } catch (exception) {
        assert.eq(exception.returnCode, MongoRunner.EXIT_UNCAUGHT);
    }

    rst.stopSet();
}

runFeatureFlagMultiversionTest('featureFlagChangeStreamPreAndPostImages', runTest);
})();
