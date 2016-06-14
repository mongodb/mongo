/**
 * Test downgrade behavior for collation:
 *  - The server should fail to start up on downgrade to 3.2 or earlier after creating a collection
 *    or index with a non-simple collation.
 *  - The server should successfully start up on downgrade to 3.2 or earlier after creating a
 *    collection or index that omits the collation.
 *  - The server should successfully start up on downgrade to 3.2 or earlier after creating a
 *    collection or index that explicitly specifies the simple collation with {locale: "simple"}.
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

    // Whenever we start "latest", we use the "enableBSON1_1" server parameter to force indices
    // created with the wiredTiger storage engine to use KeyString V0. Otherwise, downgrade will
    // fail due to creating KeyString V1 indices rather than exercising the code which prevents
    // downgrading in the presence of non-simple collations.
    var latestOptions =
        Object.extend({binVersion: "latest", setParameter: "enableBSON1_1=false"}, defaultOptions);

    var downgradeVersion = "3.2";
    var downgradeOptions = Object.extend({binVersion: downgradeVersion}, defaultOptions);

    //
    // Test that downgrade is possible after creating collections or indexes with the simple
    // collation.
    //

    // Create a collection with a simple collation on the latest version.
    var conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));

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

    // Start latest again. This time create a collection which specifies the simple collation
    // explicitly.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    testDB.dropDatabase();
    assert.commandWorked(
        testDB.createCollection("simplecollator", {collation: {locale: "simple"}}));

    // Again, we should be able to downgrade.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.neq(null,
               conn,
               "mongod should have been able to downgrade to the latest version of " +
                   downgradeVersion + "; options: " + tojson(downgradeOptions));

    // Start latest and create an index with a simple collation.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    testDB.dropDatabase();
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
    assert.commandWorked(
        testDB.simplecollation.createIndex({a: 1}, {collation: {locale: "simple"}}));

    // Ensure we can downgrade.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.neq(null,
               conn,
               "mongod should have been able to downgrade to the latest version of " +
                   downgradeVersion + "; options: " + tojson(downgradeOptions));

    //
    // Test that the server fails to start up on downgrade after creating collections or indexes
    // with a non-simple collation.
    //

    // Start latest and create a collection with a non-simple collation.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    testDB.dropDatabase();
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

    // Now downgrade should succeed.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(downgradeOptions);
    assert.neq(null,
               conn,
               "mongod should have been able to downgrade to the latest version of " +
                   downgradeVersion + "; options: " + tojson(downgradeOptions));

    //
    // Test that downgrade to 3.2.1 and 3.0 always fail. These versions do not have the code capable
    // of unsetting the collation feature bit if all indexes/collections with the collation have
    // been dropped.
    //

    // Start latest. Create an index with a non-simple collation but then drop it.
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod(latestOptions);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(latestOptions));
    testDB = conn.getDB("test");
    assert.commandWorked(testDB.nonsimple.createIndex({a: 1}, {collation: {locale: "fr"}}));
    assert.commandWorked(testDB.nonsimple.dropIndex({a: 1}));

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
