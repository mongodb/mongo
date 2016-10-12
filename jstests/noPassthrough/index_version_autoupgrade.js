/**
 * Tests whether various MongoDB commands automatically upgrade the index version of existing
 * indexes when they are rebuilt on a collection.
 */
(function() {
    "use strict";

    load("jstests/libs/get_index_helpers.js");

    function getFeatureCompatibilityVersion(conn) {
        const res = assert.commandWorked(
            conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
        return res.featureCompatibilityVersion;
    }

    var conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");
    assert.eq("3.4", getFeatureCompatibilityVersion(conn));

    var testDB = conn.getDB("test");
    assert.commandWorked(testDB.runCommand({create: "index_version_autoupgrade"}));
    var allIndexes = testDB.index_version_autoupgrade.getIndexes();
    var spec = GetIndexHelpers.findByKeyPattern(allIndexes, {_id: 1});
    assert.neq(null, spec, "Index with key pattern {_id: 1} not found: " + tojson(allIndexes));
    var defaultIndexVersion = spec.v;
    assert.lte(2, defaultIndexVersion, "Expected the defaultIndexVersion to be at least v=2");

    /**
     * Tests whether the execution of the 'commandFn' function automatically upgrades the index
     * version of existing indexes.
     *
     * The 'commandFn' function takes a single argument of the collection to act on and returns a
     * collection to validate the index versions of. Most often the 'commandFn' function returns
     * its input collection, but is able to return a reference to a different collection to support
     * testing the effects of cloning commands.
     *
     * If 'doesAutoUpgrade' is true, then this function verifies that the indexes on the returned
     * collection have been upgraded to the 'defaultIndexVersion'. If 'doesAutoUpgrade' is false,
     * then this function verifies that the indexes on the returned collection are unchanged.
     */
    function testIndexVersionAutoUpgrades(commandFn, doesAutoUpgrade) {
        testDB.dropDatabase();
        var coll = testDB.index_version_autoupgrade;

        // Create a v=1 _id index. This requires setting featureCompatibilityVersion to 3.2.
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
        assert.commandWorked(testDB.createCollection("index_version_autoupgrade"));
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.4"}));
        var allIndexes = coll.getIndexes();
        var spec = GetIndexHelpers.findByKeyPattern(allIndexes, {_id: 1});
        assert.neq(null, spec, "Index with key pattern {_id: 1} not found: " + tojson(allIndexes));
        assert.eq(1, spec.v, "Expected a v=1 index to be built: " + tojson(spec));

        assert.commandWorked(coll.createIndex({withoutAnyOptions: 1}));
        allIndexes = coll.getIndexes();
        spec = GetIndexHelpers.findByKeyPattern(allIndexes, {withoutAnyOptions: 1});
        assert.neq(
            null,
            spec,
            "Index with key pattern {withoutAnyOptions: 1} not found: " + tojson(allIndexes));
        assert.eq(defaultIndexVersion,
                  spec.v,
                  "Expected an index with the default version to be built: " + tojson(spec));

        assert.commandWorked(coll.createIndex({withV1: 1}, {v: 1}));
        allIndexes = coll.getIndexes();
        spec = GetIndexHelpers.findByKeyPattern(allIndexes, {withV1: 1});
        assert.neq(
            null, spec, "Index with key pattern {withV1: 1} not found: " + tojson(allIndexes));
        assert.eq(1, spec.v, "Expected a v=1 index to be built: " + tojson(spec));

        assert.commandWorked(coll.createIndex({withV2: 1}, {v: 2}));
        allIndexes = coll.getIndexes();
        spec = GetIndexHelpers.findByKeyPattern(allIndexes, {withV2: 1});
        assert.neq(
            null, spec, "Index with key pattern {withV2: 1} not found: " + tojson(allIndexes));
        assert.eq(2, spec.v, "Expected a v=2 index to be built: " + tojson(spec));

        var collToVerify = commandFn(coll);
        var expectedResults;

        if (doesAutoUpgrade) {
            expectedResults = [
                {keyPattern: {_id: 1}, version: defaultIndexVersion},
                {keyPattern: {withoutAnyOptions: 1}, version: defaultIndexVersion},
                {keyPattern: {withV1: 1}, version: defaultIndexVersion},
                {keyPattern: {withV2: 1}, version: defaultIndexVersion},
            ];

        } else {
            expectedResults = [
                {keyPattern: {_id: 1}, version: 1},
                {keyPattern: {withoutAnyOptions: 1}, version: defaultIndexVersion},
                {keyPattern: {withV1: 1}, version: 1},
                {keyPattern: {withV2: 1}, version: 2},
            ];
        }

        expectedResults.forEach(function(expected) {
            var allIndexes = collToVerify.getIndexes();
            var spec = GetIndexHelpers.findByKeyPattern(allIndexes, expected.keyPattern);
            assert.neq(null,
                       spec,
                       "Index with key pattern " + tojson(expected.keyPattern) + " not found: " +
                           tojson(allIndexes));
            assert.eq(expected.version,
                      spec.v,
                      "Expected index to be rebuilt with " +
                          (doesAutoUpgrade ? "the default" : "its original") + " version: " +
                          tojson(spec));
        });
    }

    // Test that the "reIndex" command upgrades all existing indexes to the latest version.
    testIndexVersionAutoUpgrades(function(coll) {
        assert.commandWorked(coll.getDB().runCommand({reIndex: coll.getName()}));
        return coll;
    }, true);

    // Test that the "repairDatabase" command doesn't upgrade existing indexes to the latest
    // version.
    testIndexVersionAutoUpgrades(function(coll) {
        assert.commandWorked(coll.getDB().runCommand({repairDatabase: 1}));
        return coll;
    }, false);

    // Test that the "compact" command doesn't upgrade existing indexes to the latest version.
    testIndexVersionAutoUpgrades(function(coll) {
        var res = coll.getDB().runCommand({compact: coll.getName()});
        if (res.ok === 0) {
            // Ephemeral storage engines don't support the "compact" command. The existing indexes
            // should remain unchanged.
            assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);
        } else {
            assert.commandWorked(res);
        }
        return coll;
    }, false);

    // Test that the "copydb" command doesn't upgrade existing indexes to the latest version.
    testIndexVersionAutoUpgrades(function(coll) {
        assert.commandWorked(coll.getDB().adminCommand({
            copydb: 1,
            fromdb: coll.getDB().getName(),
            todb: "copied",
        }));
        return coll.getDB().getSiblingDB("copied")[coll.getName()];
    }, false);

    // Test that the "cloneCollection" command doesn't upgrade existing indexes to the latest
    // version.
    var cloneConn = MongoRunner.runMongod({});
    assert.neq(null, cloneConn, "mongod was unable to start up");
    assert.eq("3.4", getFeatureCompatibilityVersion(cloneConn));
    testIndexVersionAutoUpgrades(function(coll) {
        var cloneDB = cloneConn.getDB(coll.getDB().getName());
        assert.commandWorked(cloneDB.runCommand({
            cloneCollection: coll.getFullName(),
            from: conn.host,
        }));
        return cloneDB[coll.getName()];
    }, false);
    MongoRunner.stopMongod(cloneConn);

    // Test that the "clone" command doesn't upgrade existing indexes to the latest version.
    cloneConn = MongoRunner.runMongod({});
    assert.neq(null, cloneConn, "mongod was unable to start up");
    assert.eq("3.4", getFeatureCompatibilityVersion(cloneConn));
    testIndexVersionAutoUpgrades(function(coll) {
        var cloneDB = cloneConn.getDB(coll.getDB().getName());
        assert.commandWorked(cloneDB.runCommand({
            clone: conn.host,
            fromDB: coll.getDB().getName(),
        }));
        return cloneDB[coll.getName()];
    }, false);
    MongoRunner.stopMongod(cloneConn);

    MongoRunner.stopMongod(conn);
})();
