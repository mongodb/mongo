/**
 * Test the downgrade process for indexes with path-level multikey information:
 *   - Having an index with path-level multikey information should cause versions earlier than 3.2.7
 *     to fail to start up.
 *   - It should be possible to downgrade to earlier versions of 3.2 as well as to versions of 3.0
 *     if the mongod is first downgraded to a version of 3.2 at least as new as 3.2.7.
 *
 * @tags: [requires_persistence]
 */
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    const versionDowngradeSuccess = "last-stable";

    // We cannot downgrade to any version of 3.2 earlier than 3.2.7 after creating an index while
    // running the "latest" version.
    const version32DowngradeFailure = "3.2.1";

    // We cannot downgrade to any version of 3.0 after creating an index while running the "latest"
    // version.
    const version30DowngradeFailure = "3.0";

    var dbpath = MongoRunner.dataPath + "multikey_paths_downgrade";
    resetDbpath(dbpath);

    var defaultOptions = {
        dbpath: dbpath,
        noCleanData: true,
        // We explicitly set the storage engine as part of the options because not all versions
        // being tested automatically detect it from the storage.bson file.
        storageEngine: jsTest.options().storageEngine,
    };

    if (defaultOptions.storageEngine === "mmapv1") {
        // Path-level multikey tracking is supported for all storage engines that use the KVCatalog.
        // MMAPv1 is the only storage engine that does not.
        //
        // TODO SERVER-22727: Store path-level multikey information in MMAPv1 index catalog.
        print("Skipping test because mmapv1 doesn't yet support path-level multikey tracking");
        return;
    }

    /**
     * Returns whether the index with the specified key pattern is multikey and its path-level
     * multikey information, if present.
     */
    function extractMultikeyInfoFromExplainOutput(db, keyPattern) {
        var explain = db.runCommand({explain: {find: "multikey_paths", hint: keyPattern}});
        assert.commandWorked(explain);

        assert(planHasStage(explain.queryPlanner.winningPlan, "IXSCAN"),
               "expected stage to be present: " + tojson(explain));
        var stage = getPlanStage(explain.queryPlanner.winningPlan, "IXSCAN");
        assert(stage.hasOwnProperty("isMultiKey"),
               "expected IXSCAN to have isMultiKey property: " + tojson(stage));

        var multikeyInfo = {
            isMultiKey: stage.isMultiKey,
        };
        if (stage.hasOwnProperty("multiKeyPaths")) {
            multikeyInfo.multiKeyPaths = stage.multiKeyPaths;
        }
        return multikeyInfo;
    }

    //
    // Create a multikey index on 3.2.
    //
    var options = Object.extend({binVersion: version32DowngradeFailure}, defaultOptions);
    var conn = MongoRunner.runMongod(options);
    assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(options));

    var testDB = conn.getDB("test");
    assert.commandWorked(testDB.multikey_paths.createIndex({createdOn32: 1}));
    assert.writeOK(testDB.multikey_paths.insert({createdOn32: [1, 2, 3]}));

    // The index created on 3.2 shouldn't have path-level multikey information, but it should be
    // marked as multikey.
    var multikeyInfo = extractMultikeyInfoFromExplainOutput(testDB, {createdOn32: 1});
    assert.eq(true, multikeyInfo.isMultiKey, tojson(multikeyInfo));
    assert(!multikeyInfo.hasOwnProperty("multiKeyPaths"), tojson(multikeyInfo));

    //
    // Upgrade from 3.2 to the "latest" version.
    //
    MongoRunner.stopMongod(conn);

    options = Object.extend({binVersion: "latest"}, defaultOptions);
    conn = MongoRunner.runMongod(options);
    assert.neq(null,
               conn,
               "mongod should have been able to upgrade directly from " +
                   version32DowngradeFailure + " to the latest version; options: " +
                   tojson(options));
    testDB = conn.getDB("test");

    // The index created on 3.2 shouldn't have path-level multikey information, but it should be
    // marked as multikey.
    multikeyInfo = extractMultikeyInfoFromExplainOutput(testDB, {createdOn32: 1});
    assert.eq(true, multikeyInfo.isMultiKey, tojson(multikeyInfo));
    assert(!multikeyInfo.hasOwnProperty("multiKeyPaths"), tojson(multikeyInfo));

    //
    // Create a multikey index on the "latest" version.
    //
    assert.commandWorked(testDB.multikey_paths.createIndex({createdOnLatest: 1}));
    assert.writeOK(testDB.multikey_paths.insert({createdOnLatest: [1, 2, 3]}));

    // The index created on the "latest" version should have path-level multikey information.
    multikeyInfo = extractMultikeyInfoFromExplainOutput(testDB, {createdOnLatest: 1});
    assert.eq(true, multikeyInfo.isMultiKey, tojson(multikeyInfo));
    assert.eq(
        {createdOnLatest: ["createdOnLatest"]}, multikeyInfo.multiKeyPaths, tojson(multikeyInfo));

    //
    // Attempt to downgrade from the "latest" version to 3.2.
    //
    MongoRunner.stopMongod(conn);
    options = Object.extend({binVersion: version32DowngradeFailure}, defaultOptions);
    conn = MongoRunner.runMongod(options);
    assert.eq(null,
              conn,
              "mongod shouldn't have been able to downgrade from the latest version to " +
                  version32DowngradeFailure + " after creating an index on the latest version;" +
                  " options: " + tojson(options));

    //
    // Attempt to downgrade from the "latest" version to 3.0.
    //
    options = Object.extend({binVersion: version30DowngradeFailure}, defaultOptions);
    conn = MongoRunner.runMongod(options);
    assert.eq(null,
              conn,
              "mongod shouldn't have been able to downgrade from the latest version to " +
                  version30DowngradeFailure + " after creating an index on the latest version;" +
                  " options: " + tojson(options));

    //
    // Downgrade from the "latest" version to the "last-stable" version.
    //
    options = Object.extend({binVersion: versionDowngradeSuccess}, defaultOptions);
    conn = MongoRunner.runMongod(options);
    assert.neq(null,
               conn,
               "mongod should have been able to downgrade from the latest version to the last" +
                   " stable version, even after creating an index on the latest version;" +
                   " options: " + tojson(options));
    testDB = conn.getDB("test");

    //
    // Upgrade from the "last-stable" version to the "latest" version.
    //
    MongoRunner.stopMongod(conn);
    options = Object.extend({binVersion: "latest"}, defaultOptions);
    conn = MongoRunner.runMongod(options);
    assert.neq(
        null,
        conn,
        "mongod should have been able to upgrade from the last-stable version to the latest" +
            " version; options: " + tojson(options));
    testDB = conn.getDB("test");

    // The index created on 3.2 shouldn't have path-level multikey information.
    multikeyInfo = extractMultikeyInfoFromExplainOutput(testDB, {createdOn32: 1});
    assert.eq(true, multikeyInfo.isMultiKey, tojson(multikeyInfo));
    assert(!multikeyInfo.hasOwnProperty("multiKeyPaths"), tojson(multikeyInfo));

    // The index created on the "latest" version should no longer have path-level multikey
    // information either; the path-level multikey information should have been deleted when we
    // downgraded to the "last-stable" version.
    multikeyInfo = extractMultikeyInfoFromExplainOutput(testDB, {createdOnLatest: 1});
    assert.eq(true, multikeyInfo.isMultiKey, tojson(multikeyInfo));
    assert(!multikeyInfo.hasOwnProperty("multiKeyPaths"), tojson(multikeyInfo));

    //
    // Downgrade from the "latest" version to 3.2.
    //
    MongoRunner.stopMongod(conn);
    options = Object.extend({binVersion: version32DowngradeFailure}, defaultOptions);
    conn = MongoRunner.runMongod(options);
    assert.neq(null,
               conn,
               "mongod should have been able to downgrade from the latest version to 3.2 because" +
                   " no index with path-level multikey information exists; options: " +
                   tojson(options));

    //
    // Downgrade from 3.2 to 3.0.
    //
    MongoRunner.stopMongod(conn);
    options = Object.extend({binVersion: version30DowngradeFailure}, defaultOptions);
    conn = MongoRunner.runMongod(options);
    assert.neq(
        null,
        conn,
        "mongod should have been able to downgrade from 3.2 to 3.0; options: " + tojson(options));

    MongoRunner.stopMongod(conn);
})();
