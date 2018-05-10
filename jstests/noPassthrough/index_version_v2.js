/**
 * Tests that index version v=2 is the default.
 *
 * Additionally, this file tests that index version v=2 is required to create an index with a
 * collation and that index version v=2 is required to index decimal data on storage engines using
 * the KeyString format.
 */
(function() {
    "use strict";

    const storageEnginesUsingKeyString = new Set(["wiredTiger", "inMemory", "rocksdb"]);

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

    const testDB = conn.getDB("test");
    const storageEngine = testDB.serverStatus().storageEngine.name;

    //
    // Index version v=2
    //

    testDB.dropDatabase();

    // Test that the _id index of a collection is created with v=2 by default.
    assert.commandWorked(testDB.runCommand({create: "index_version"}));
    let indexSpec = getIndexSpecByName(testDB.index_version, "_id_");
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Test that an index created on an existing collection is created with v=2 by default.
    assert.commandWorked(testDB.index_version.createIndex({defaultToV2: 1}, {name: "defaultToV2"}));
    indexSpec = getIndexSpecByName(testDB.index_version, "defaultToV2");
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Test that creating an index with v=2 succeeds.
    assert.commandWorked(testDB.index_version.createIndex({withV2: 1}, {v: 2, name: "withV2"}));
    indexSpec = getIndexSpecByName(testDB.index_version, "withV2");
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Test that creating a collection with a non-simple default collation succeeds.
    assert.commandWorked(testDB.runCommand({create: "collation", collation: {locale: "en"}}));
    indexSpec = getIndexSpecByName(testDB.collation, "_id_");
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Test that creating an index with a non-simple collation succeeds.
    assert.commandWorked(
        testDB.collation.createIndex({str: 1}, {name: "withCollation", collation: {locale: "fr"}}));
    indexSpec = getIndexSpecByName(testDB.collation, "withCollation");
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Test that indexing decimal data succeeds.
    assert.writeOK(testDB.decimal.insert({_id: new NumberDecimal("42")}));

    //
    // Index version v=1
    //

    testDB.dropDatabase();

    // Test that creating an index with v=1 succeeds.
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
    // Index version v=0
    //

    testDB.dropDatabase();

    // Test that attempting to create an index with v=0 returns an error.
    assert.commandFailed(testDB.index_version.createIndex({withV0: 1}, {v: 0}));

    //
    // Index version v=3
    //

    testDB.dropDatabase();

    // Test that attempting to create an index with v=3 returns an error.
    assert.commandFailed(testDB.index_version.createIndex({withV3: 1}, {v: 3}));
    MongoRunner.stopMongod(conn);
})();
