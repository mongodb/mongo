/**
 * Test downgrade behavior for collation:
 *
 *  - The server should start up on downgrade to 3.2 after creating a collection without specifying
 *    the "collation" option when the featureCompatibilityVersion is 3.2.
 *
 *  - The server should start up on downgrade to 3.2 after creating an index without specifying the
 *    "collation" option when the featureCompatibilityVersion is 3.2.
 *
 *  - The server should fail to start up on downgrade to 3.2 after creating a collection or index
 *    with a simple collation until all indexes are re-indexed with version v=1.
 *
 *  - The server should fail to start up on downgrade to 3.2 after creating a collection or index
 *    with a non-simple collation until those collections and indexes are dropped and any remaining
 *    indexes are re-indexed with version v=1.
 *
 *  - The server should fail to start up on downgrade to 3.0 and earlier versions of 3.2 after
 *    creating a collection or index with a non-simple collation, even after those collections and
 *    indexes are dropped.
 */
(function() {
    "use strict";

    var dbpath = MongoRunner.dataPath + "collation_downgrade";
    resetDbpath(dbpath);

    var defaultOptions = {
        dbpath: dbpath,
        noCleanData: true,
        // We explicitly set the storage engine as part of the options because not all versions
        // being tested automatically detect it from the storage.bson file.
        storageEngine: jsTest.options().storageEngine || "wiredTiger",
    };

    var latestOptions = Object.extend({binVersion: "latest"}, defaultOptions);

    var downgradeVersion = "3.2";
    var downgradeOptions = Object.extend({binVersion: downgradeVersion}, defaultOptions);

    //
    // Test that the server starts up on downgrade from the latest version to 3.2 after creating a
    // collection without specifying the "collation" option.
    //

    var conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));

    // We set the featureCompatibilityVersion to 3.2 so that the default index version becomes v=1.
    // We do this prior to writing any data to the server so that any indexes created during this
    // test are compatible with 3.2. This effectively allows us to emulate upgrading to the latest
    // version with existing data files and then trying to downgrade back to 3.2.
    assert.commandWorked(conn.getDB("admin").runCommand({setFeatureCompatibilityVersion: "3.2"}));

    // Create a collection on the latest version without specifying the "collation" option.
    var testDB = conn.getDB("test");
    testDB.dropDatabase();
    assert.commandWorked(testDB.createCollection("simplecollator"));

    // We should be able to downgrade, since the collection has no collation.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.neq(null,
               conn,
               "mongod should have been able to downgrade to the latest version of " +
                   downgradeVersion + "; options: " + tojson(downgradeOptions));

    //
    // Test that the server starts up on downgrade from the latest version to 3.2 after creating an
    // index without specifying the "collation" option.
    //

    // Create an index on the latest version without specifying the "collation" option.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    assert.commandWorked(testDB.simplecollation.createIndex({a: 1}));

    // We should be able to downgrade, since neither the index nor the collection has a collation.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.neq(null,
               conn,
               "mongod should have been able to downgrade to the latest version of " +
                   downgradeVersion + "; options: " + tojson(downgradeOptions));

    //
    // Test that the server fails to start up on downgrade from the latest version to 3.2 after
    // creating a collection with a simple collation. Downgrade should succeed after setting the
    // featureCompatibilityVersion to 3.2 and re-indexing the collection.
    //

    // Start latest again. This time create a collection which specifies the simple collation
    // explicitly.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    testDB.dropDatabase();

    // We set the featureCompatibilityVersion back to 3.4 in order to use the "collation" option.
    assert.commandWorked(conn.getDB("admin").runCommand({setFeatureCompatibilityVersion: "3.4"}));
    assert.commandWorked(
        testDB.createCollection("simplecollator", {collation: {locale: "simple"}}));

    // Downgrade should fail because we have an _id index with v=2 on the "test.simplecollator"
    // collection.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.eq(null,
              conn,
              "mongod shouldn't have been able to downgrade to the latest version of " +
                  downgradeVersion + "; options: " + tojson(downgradeOptions));

    // Start latest and create an index with a simple collation.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    testDB.dropDatabase();

    // We set the featureCompatibilityVersion to 3.2 so that the default index version becomes v=1.
    assert.commandWorked(conn.getDB("admin").runCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandWorked(testDB.simplecollation.createIndex({a: 1}));

    // Ensure we can downgrade.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.neq(null,
               conn,
               "mongod should have been able to downgrade to the latest version of " +
                   downgradeVersion + "; options: " + tojson(downgradeOptions));

    // Start latest and create an index which specifies the simple collation explicitly.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    testDB.dropDatabase();

    // We set the featureCompatibilityVersion back to 3.4 in order to use the "collation" option.
    assert.commandWorked(conn.getDB("admin").runCommand({setFeatureCompatibilityVersion: "3.4"}));
    assert.commandWorked(
        testDB.simplecollation.createIndex({a: 1}, {collation: {locale: "simple"}}));

    // Downgrade should fail because we have v=2 indexes on the "test.simplecollator" collection.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.eq(null,
              conn,
              "mongod shouldn't have been able to downgrade to the latest version of " +
                  downgradeVersion + "; options: " + tojson(downgradeOptions));

    // Start latest and re-index the "test.simplecollation" collection. We set the
    // featureCompatibilityVersion to 3.2 so that the default index version becomes v=1.
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandWorked(testDB.simplecollation.reIndex());

    // Now downgrade should succeed.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.neq(null,
               conn,
               "mongod should have been able to downgrade to the latest version of " +
                   downgradeVersion + "; options: " + tojson(downgradeOptions));

    //
    // Test that the server fails to start up on downgrade from the latest version to 3.2 after
    // creating a collection with a non-simple collation. Downgrade should succeed after dropping
    // the collection with the non-simple collection, setting the featureCompatibilityVersion to
    // 3.2, and re-indexing any collections with a simple collation.
    //

    // Start latest and create a collection with a non-simple collation.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    testDB.dropDatabase();

    // We set the featureCompatibilityVersion back to 3.4 in order to use the "collation" option.
    assert.commandWorked(conn.getDB("admin").runCommand({setFeatureCompatibilityVersion: "3.4"}));
    assert.commandWorked(testDB.createCollection("simplecollator"));
    assert.commandWorked(testDB.createCollection("nonsimple", {collation: {locale: "fr"}}));

    // Downgrade should fail.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.eq(null,
              conn,
              "mongod shouldn't have been able to downgrade to the latest version of " +
                  downgradeVersion + "; options: " + tojson(downgradeOptions));

    // Start latest and drop the collection with the non-simple collation.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    testDB.nonsimple.drop();

    // Downgrade should still fail because we have an _id index with v=2 on the
    // "test.simplecollator" collection.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.eq(null,
              conn,
              "mongod shouldn't have been able to downgrade to the latest version of " +
                  downgradeVersion + "; options: " + tojson(downgradeOptions));

    // Start latest and re-index the "test.simplecollator" collection. We set the
    // featureCompatibilityVersion to 3.2 so that the default index version becomes v=1.
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandWorked(testDB.simplecollator.reIndex());

    // Now downgrade should succeed.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.neq(null,
               conn,
               "mongod should have been able to downgrade to the latest version of " +
                   downgradeVersion + "; options: " + tojson(downgradeOptions));

    // Start latest and create an index with a non-simple collation.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    testDB.dropDatabase();

    // We set the featureCompatibilityVersion back to 3.4 in order to use the "collation" option.
    assert.commandWorked(conn.getDB("admin").runCommand({setFeatureCompatibilityVersion: "3.4"}));
    assert.commandWorked(testDB.simplecollator.createIndex({a: 1}));
    assert.commandWorked(testDB.nonsimple.createIndex({a: 1}, {collation: {locale: "fr"}}));

    // Downgrade should fail.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.eq(null,
              conn,
              "mongod shouldn't have been able to downgrade to the latest version of " +
                  downgradeVersion + "; options: " + tojson(downgradeOptions));

    // Start latest and drop the index with the non-simple collation.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    assert.commandWorked(testDB.nonsimple.dropIndex({a: 1}));

    // Downgrade should still fail because we have v=2 indexes on the "test.simplecollator"
    // collection.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.eq(null,
              conn,
              "mongod shouldn't have been able to downgrade to the latest version of " +
                  downgradeVersion + "; options: " + tojson(downgradeOptions));

    // Start latest and re-index the "test.simplecollator" and "test.nonsimple" collections. We set
    // the featureCompatibilityVersion to 3.2 so that the default index version becomes v=1.
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandWorked(testDB.simplecollator.reIndex());
    assert.commandWorked(testDB.nonsimple.reIndex());

    // Now downgrade should succeed.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.neq(null,
               conn,
               "mongod should have been able to downgrade to the latest version of " +
                   downgradeVersion + "; options: " + tojson(downgradeOptions));

    //
    // Test that the server fails to start up on downgrade from the latest version to 3.2.1 and from
    // the latest version to 3.0 after creating an index with a non-simple collation. Downgrade
    // shouldn't succeed under any circumstances because the aforementioned versions don't have code
    // capable of unsetting the collation feature bit. In particular, downgrade from the latest
    // version to 3.2.1 and the latest version to 3.0 should fail even if no v=2 indexes currently
    // exist in the data files.
    //

    // Start latest. Create an index with a non-simple collation but then drop it.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");

    // We set the featureCompatibilityVersion back to 3.4 in order to use the "collation" option.
    assert.commandWorked(conn.getDB("admin").runCommand({setFeatureCompatibilityVersion: "3.4"}));
    assert.commandWorked(testDB.nonsimple.createIndex({a: 1}, {collation: {locale: "fr"}}));
    assert.commandWorked(testDB.nonsimple.dropIndex({a: 1}));

    // Re-index the "test.nonsimple" collection. We set the featureCompatibilityVersion to 3.2 so
    // that the default index version becomes v=1.
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.commandWorked(testDB.nonsimple.reIndex());

    var downgradeAlwaysFailsVersion = "3.2.1";
    var downgradeAlwaysFailsOptions =
        Object.extend({binVersion: downgradeAlwaysFailsVersion}, defaultOptions);

    // Downgrade should fail.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeAlwaysFailsOptions);
    assert.eq(null,
              conn,
              "mongod shouldn't have been able to downgrade to " + downgradeAlwaysFailsVersion +
                  "; options: " + tojson(downgradeAlwaysFailsOptions));

    // Downgrade to 3.0 should also fail.
    downgradeAlwaysFailsVersion = "3.0";
    downgradeAlwaysFailsOptions =
        Object.extend({binVersion: downgradeAlwaysFailsVersion}, defaultOptions);
    conn = MongoRunner.runMongod(downgradeAlwaysFailsOptions);
    assert.eq(null,
              conn,
              "mongod shouldn't have been able to downgrade to " + downgradeAlwaysFailsVersion +
                  "; options: " + tojson(downgradeAlwaysFailsOptions));
})();
