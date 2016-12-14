/**
 * Tests the interaction of the default index version and the featureCompatibilityVersion:
 *   - Index version v=2 is the default when the featureCompatibilityVersion is 3.4
 *   - Index version v=1 is the default when the featureCompatibilityVersion is 3.2
 *
 * Additionally, this file tests that index version v=2 is required to create an index with a
 * collation and that index version v=2 is required to index decimal data on storage engines using
 * the KeyString format.
 */
(function() {
    "use strict";

    const storageEnginesUsingKeyString = new Set(["wiredTiger", "inMemory", "rocksdb"]);

    function getFeatureCompatibilityVersion(conn) {
        const res = assert.commandWorked(
            conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}));
        return res.featureCompatibilityVersion;
    }

    function getIndexSpecByName(coll, indexName) {
        const indexes = coll.getIndexes();
        const indexesFilteredByName = indexes.filter(spec => spec.name === indexName);
        assert.eq(1,
                  indexesFilteredByName.length,
                  "index '" + indexName + "' not found: " + tojson(indexes));
        return indexesFilteredByName[0];
    }

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");
    assert.eq("3.4", getFeatureCompatibilityVersion(conn));

    const testDB = conn.getDB("test");
    const storageEngine = testDB.serverStatus().storageEngine.name;

    //
    // Index version v=2 and featureCompatibilityVersion=3.4
    //

    testDB.dropDatabase();

    // Test that the _id index of a collection is created with v=2 when the
    // featureCompatibilityVersion is 3.4.
    assert.commandWorked(testDB.runCommand({create: "index_version"}));
    let indexSpec = getIndexSpecByName(testDB.index_version, "_id_");
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Test that an index created on an existing collection is created with v=2 when the
    // featureCompatibilityVersion is 3.4.
    assert.commandWorked(testDB.index_version.createIndex({defaultToV2: 1}, {name: "defaultToV2"}));
    indexSpec = getIndexSpecByName(testDB.index_version, "defaultToV2");
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Test that creating an index with v=2 succeeds when the featureCompatibilityVersion is 3.4.
    assert.commandWorked(testDB.index_version.createIndex({withV2: 1}, {v: 2, name: "withV2"}));
    indexSpec = getIndexSpecByName(testDB.index_version, "withV2");
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Test that creating a collection with a non-simple default collation succeeds when the
    // featureCompatibilityVersion is 3.4.
    assert.commandWorked(testDB.runCommand({create: "collation", collation: {locale: "en"}}));
    indexSpec = getIndexSpecByName(testDB.collation, "_id_");
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Test that creating an index with a non-simple collation succeeds when the
    // featureCompatibilityVersion is 3.4.
    assert.commandWorked(
        testDB.collation.createIndex({str: 1}, {name: "withCollation", collation: {locale: "fr"}}));
    indexSpec = getIndexSpecByName(testDB.collation, "withCollation");
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Test that indexing decimal data succeeds when the featureCompatibilityVersion is 3.4.
    assert.writeOK(testDB.decimal.insert({_id: new NumberDecimal("42")}));

    //
    // Index version v=1 and featureCompatibilityVersion=3.4
    //

    testDB.dropDatabase();

    // Test that creating an index with v=1 succeeds when the featureCompatibilityVersion is 3.4.
    assert.commandWorked(testDB.index_version.createIndex({withV1: 1}, {v: 1, name: "withV1"}));
    indexSpec = getIndexSpecByName(testDB.index_version, "withV1");
    assert.eq(1, indexSpec.v, tojson(indexSpec));

    // Test that creating an index with v=1 and a simple collation returns an error.
    assert.commandFailed(
        testDB.collation.createIndex({str: 1}, {v: 1, collation: {locale: "simple"}}));

    // Test that creating an index with v=1 and a non-simple collation returns an error.
    assert.commandFailed(
        testDB.collation.createIndex({str: 1}, {v: 1, collation: {locale: "en", strength: 2}}));

    // Test that creating an index with v=1 and a simple collation on a collection with a non-simple
    // default collation returns an error.
    testDB.collation.drop();
    assert.commandWorked(testDB.runCommand({create: "collation", collation: {locale: "en"}}));
    assert.commandFailed(
        testDB.collation.createIndex({str: 1}, {v: 1, collation: {locale: "simple"}}));

    // Test that creating an index with v=1 and a non-simple collation on a collection with a
    // non-simple default collation returns an error.
    testDB.collation.drop();
    assert.commandWorked(testDB.runCommand({create: "collation", collation: {locale: "en"}}));
    assert.commandFailed(
        testDB.collation.createIndex({str: 1}, {v: 1, collation: {locale: "en", strength: 2}}));

    // Test that indexing decimal data with a v=1 index returns an error on storage engines using
    // the KeyString format.
    assert.commandWorked(testDB.decimal.createIndex({num: 1}, {v: 1}));
    if (storageEnginesUsingKeyString.has(storageEngine)) {
        assert.writeErrorWithCode(testDB.decimal.insert({num: new NumberDecimal("42")}),
                                  ErrorCodes.UnsupportedFormat);
    } else {
        assert.writeOK(testDB.decimal.insert({num: new NumberDecimal("42")}));
    }

    //
    // Index version v=0 and featureCompatibilityVersion=3.4
    //

    testDB.dropDatabase();

    // Test that attempting to create an index with v=0 returns an error.
    assert.commandFailed(testDB.index_version.createIndex({withV0: 1}, {v: 0}));

    //
    // Index version v=3 and featureCompatibilityVersion=3.4
    //

    testDB.dropDatabase();

    // Test that attempting to create an index with v=3 returns an error.
    assert.commandFailed(testDB.index_version.createIndex({withV3: 1}, {v: 3}));

    //
    // Index version v=1 and featureCompatibilityVersion=3.2
    //

    // Set the featureCompatibilityVersion to 3.2.
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "3.2"}));
    assert.eq("3.2", getFeatureCompatibilityVersion(conn));
    testDB.dropDatabase();

    // Test that the _id index of a collection is created with v=1 when the
    // featureCompatibilityVersion is 3.2.
    assert.commandWorked(testDB.runCommand({create: "index_version"}));
    indexSpec = getIndexSpecByName(testDB.index_version, "_id_");
    assert.eq(1, indexSpec.v, tojson(indexSpec));

    // Test that the _id index of a collection is created with v=1 when the
    // featureCompatibilityVersion is 3.2 and an "idIndex" spec is provided without a version.
    testDB.index_version.drop();
    assert.commandWorked(
        testDB.runCommand({create: "index_version", idIndex: {key: {_id: 1}, name: "_id_"}}));
    indexSpec = getIndexSpecByName(testDB.index_version, "_id_");
    assert.eq(1, indexSpec.v, tojson(indexSpec));

    // Test that an index created on an existing collection is created with v=1 when the
    // featureCompatibilityVersion is 3.2.
    assert.commandWorked(testDB.index_version.createIndex({defaultToV1: 1}, {name: "defaultToV1"}));
    indexSpec = getIndexSpecByName(testDB.index_version, "defaultToV1");
    assert.eq(1, indexSpec.v, tojson(indexSpec));

    // Test that creating an index with v=1 succeeds when the featureCompatibilityVersion is 3.2.
    assert.commandWorked(testDB.index_version.createIndex({withV1: 1}, {v: 1, name: "withV1"}));
    indexSpec = getIndexSpecByName(testDB.index_version, "withV1");
    assert.eq(1, indexSpec.v, tojson(indexSpec));

    // Test that creating a collection with a non-simple default collation returns an error when the
    // featureCompatibilityVersion is 3.2.
    assert.commandFailed(testDB.runCommand({create: "collation", collation: {locale: "en"}}));

    // Test that creating an index with a non-simple collation returns an error when the
    // featureCompatibilityVersion is 3.2.
    assert.commandFailed(testDB.collation.createIndex({str: 1}, {collation: {locale: "fr"}}));

    // Test that creating a collection with a non-simple default collation and without an _id index
    // succeeds when the featureCompatibilityVersion is 3.2.
    testDB.collation.drop();
    assert.commandFailed(
        testDB.runCommand({create: "collation", collation: {locale: "en"}, autoIndexId: false}));

    // Test that creating a collection with a simple default collation and without an _id index
    // succeeds when the featureCompatibilityVersion is 3.2.
    testDB.collation.drop();
    assert.commandFailed(testDB.runCommand(
        {create: "collation", collation: {locale: "simple"}, autoIndexId: false}));

    // Test that creating a collection with a simple default collation succeeds when the
    // featureCompatibilityVersion is 3.2.
    testDB.collation.drop();
    assert.commandFailed(testDB.runCommand({create: "collation", collation: {locale: "simple"}}));

    // Test that creating an index with a simple collation returns an error when the
    // featureCompatibilityVersion is 3.2.
    assert.commandFailed(testDB.collation.createIndex(
        {str: 1}, {name: "withSimpleCollation", collation: {locale: "simple"}}));

    // Test that inserting decimal data (both indexed and unindexed) returns an error when the
    // featureCompatibilityVersion is 3.2.
    assert.writeErrorWithCode(testDB.decimal.insert({_id: new NumberDecimal("42")}),
                              ErrorCodes.InvalidBSON);
    assert.writeErrorWithCode(testDB.decimal.insert({num: new NumberDecimal("42")}),
                              ErrorCodes.InvalidBSON);

    //
    // Index version v=2 and featureCompatibilityVersion=3.2
    //

    testDB.dropDatabase();

    // Test that attempting to create an index with v=2 when the featureCompatibilityVersion is 3.2
    // returns an error.
    assert.commandFailed(testDB.index_version.createIndex({withV2: 1}, {v: 2, name: "withV2"}));

    testDB.dropDatabase();

    // Test that attempting to create an _id index with v=2 when the featureCompatibilityVersion is
    // 3.2 returns an error.
    assert.commandFailed(
        testDB.runCommand({create: "index_version", idIndex: {key: {_id: 1}, name: "_id_", v: 2}}));

    //
    // Index version v=0 and featureCompatibilityVersion=3.2
    //

    testDB.dropDatabase();

    // Test that attempting to create an index with v=0 returns an error.
    assert.commandFailed(testDB.index_version.createIndex({withV0: 1}, {v: 0}));

    //
    // Index version v=3 and featureCompatibilityVersion=3.2
    //

    testDB.dropDatabase();

    // Test that attempting to create an index with v=3 returns an error.
    assert.commandFailed(testDB.index_version.createIndex({withV3: 1}, {v: 3}));
})();
