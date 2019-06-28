/**
 * Regression test that runs validate to test KeyString changes across 4.2 and the current
 * version as specified in SERVER-41908.
 *
 * - First, start mongod in 4.2.
 * - For each index create a new collection in testDb, inserting documents and finally an index is
 *   created.
 * - After all indexes and collections are added, shutdown mongod.
 * - Restart the database as the current version.
 * - Run Validate.
 * - Remove all collections.
 * - Recreate all the indexes.
 * - Shuwdown mongod.
 * - Restart mongod in 4.2.
 * - Run Validate.
 *
 *
 * The following index types are tested:
 * - btree
 * - 2d
 * - geoHaystack
 * - 2dsphere
 * - text
 * - *hashed
 * - *wildcard
 * * these indexes are only created as v2 non-unique because they are not available unique or in v1
 *
 * For each index type, a v1 unique, v2 unique, v1 non-unique and v2 non-unique index
 * is considered except for hashed and wildcard, which only consider the v2 non-unique case.
 */
(function() {
    'use strict';
    load('jstests/hooks/validate_collections.js');

    // ----- Config
    // The number of documents created for each collection
    const numDocs = 100;

    const indexTypes = [
        {
          // an indicator of what the index is
          indexName: "BTreeIndex",
          // This function is called to create documents, which are then inserted into the
          // collection.
          createDoc: i => ({a: i}),
          // the options given to the .createIndex method
          // i.e. collection.createIndex(creationOptions)
          creationOptions: {a: 1},
          // This optional parameter specifies extra options to give to createIndex.
          // In the code, collection.createIndexes(creationOptions, createIndexOptions)
          // is called.
          createIndexOptions: {}
        },
        {indexName: "2d", createDoc: i => ({loc: [i, i]}), creationOptions: {loc: "2d"}},
        {
          indexName: "hayStack",
          createDoc: i => ({loc: {lng: (i / 2.0) * (i / 2.0), lat: (i / 2.0)}, a: i}),
          creationOptions: {loc: "geoHaystack", a: 1},
          createIndexOptions: {bucketSize: 1}
        },
        {
          indexName: "2dSphere",
          createDoc: i => {
              if (i == 0)
                  return {
                      "loc": {
                          "type": "Polygon",
                          "coordinates": [[[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]]
                      }
                  };
              else
                  return (
                      {loc: {type: "Point", coordinates: [(i / 10.0) * (i / 10.0), (i / 10.0)]}});
          },
          creationOptions: {loc: "2dsphere"}
        },
        {indexName: "text", createDoc: i => ({a: "a".repeat(i + 1)}), creationOptions: {a: "text"}},
        {indexName: "hashed", createDoc: i => ({a: i}), creationOptions: {a: "hashed"}},
        {
          indexName: "wildCard",
          createDoc: i => {
              if (i == 0)
                  return {};
              else if (i == 1)
                  return {a: null};
              else if (i == 2)
                  return {a: {}};
              else if (i % 2 == 0)
                  return {a: {b: i}};
              else
                  return {a: [i]};
          },
          creationOptions: {"$**": 1}
        }
    ];
    // -----

    const dbpath = MongoRunner.dataPath + 'keystring_index';
    resetDbpath(dbpath);

    const defaultOptions = {dbpath};

    const version42 = {binVersion: '4.2', testCollection: 'testdb'};
    let mongodOptions42 = Object.extend({binVersion: version42.binVersion}, defaultOptions);
    let mongodOptionsCurrent = Object.extend({binVersion: 'latest'}, defaultOptions);

    // We will first start up an old binary version database, populate the database,
    // then upgrade and validate.

    // Start up an old binary version mongod.
    let conn = MongoRunner.runMongod(mongodOptions42);

    assert.neq(
        null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptions42));

    let testDb = conn.getDB('test');
    assert.neq(null, testDb, 'testDb not found. conn.getDB(\'test\') returned null');

    populateDb(testDb);
    MongoRunner.stopMongod(conn);

    // Restart the mongod with the latest binary version on the old version's data files.
    conn = MongoRunner.runMongod(mongodOptionsCurrent);
    assert.neq(null, conn, 'mongod was unable to start with the latest version');
    testDb = conn.getDB('test');

    // Validate all the indexes.
    validateCollections(testDb, {full: true});

    // Next, we will repopulate the database with the latest version then downgrade and run
    // validate.
    dropAllUserCollections(testDb);
    populateDb(testDb);
    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod(mongodOptions42);
    assert.neq(
        null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptions42));

    testDb = conn.getDB('test');
    assert.neq(null, testDb, 'testDb not found. conn.getDB(\'test\') returned null');

    validateCollections(testDb, {full: true});
    MongoRunner.stopMongod(conn);

    // ----------------- Utilities

    // Populate the database using the config specified by the indexTypes array.
    function populateDb(testDb) {
        // Create a new collection and index for each indexType in the array.
        for (let i = 0; i < indexTypes.length; i++) {
            const indexOptions = indexTypes[i];
            // Try unique and non-unique.
            for (const unique in [true, false]) {
                // Try index-version 1 and 2.
                for (let indexVersion = 1; indexVersion <= 2; indexVersion++) {
                    let indexName = indexOptions.indexName;

                    // We only run V2 non-unique for hashed and wildCard because they don't exist in
                    // v1.
                    if ((indexName == "hashed" || indexName == "wildCard") &&
                        (unique == true || indexVersion == 1))
                        continue;

                    indexName += unique == true ? "Unique" : "NotUnique";
                    indexName += `Version${indexVersion}`;
                    let collectionName = version42.testCollection + indexName;
                    print(`${indexName}: Creating Collection`);
                    assert.commandWorked(testDb.createCollection(collectionName));

                    print(`${indexName}: Inserting Documents`);
                    if (unique)
                        insertDocumentsUnique(
                            testDb[collectionName], numDocs, indexOptions.createDoc);
                    else
                        insertDocumentsNotUnique(
                            testDb[collectionName], numDocs, indexOptions.createDoc);

                    let extraCreateIndexOptions = {
                        name: indexName,
                        v: indexVersion,
                        unique: unique == true
                    };

                    if ("createIndexOptions" in indexOptions)
                        extraCreateIndexOptions =
                            Object.extend(extraCreateIndexOptions, indexOptions.createIndexOptions);
                    print(JSON.stringify(extraCreateIndexOptions));
                    print(`${indexName}: Creating Index`);
                    assert.commandWorked(testDb[collectionName].createIndex(
                        indexOptions.creationOptions, extraCreateIndexOptions));

                    // Assert that the correct index type was created.
                    let indexSpec = getIndexSpecByName(testDb[collectionName], indexName);
                    assert.eq(indexVersion, indexSpec.v, tojson(indexSpec));
                }
            }
        }
    }

    // Drop all user created collections in a database.
    function dropAllUserCollections(testDb) {
        testDb.getCollectionNames().forEach((collName) => {
            if (!collName.startsWith("system.")) {
                testDb[collName].drop();
            }
        });
    }

    function getIndexSpecByName(coll, indexName) {
        const indexes = coll.getIndexes();
        const indexesFilteredByName = indexes.filter(spec => spec.name === indexName);
        assert.eq(1,
                  indexesFilteredByName.length,
                  "index '" + indexName + "' not found: " + tojson(indexes));
        return indexesFilteredByName[0];
    }

    function fibonacci(num, memo) {
        memo = memo || {};

        if (memo[num])
            return memo[num];
        if (num <= 1)
            return 1;

        memo[num] = fibonacci(num - 1, memo) + fibonacci(num - 2, memo);
        return memo[num];
    }

    // Insert numDocs documents into the collection by calling getDoc.
    // NOTE: Documents returned from getDoc are inserted more than once.
    function insertDocumentsNotUnique(collection, numDocs, getDoc) {
        let fibNum = 0;
        // fibonacci numbers are used because the fibonnaci sequence is a
        // exponentially growing sequence that allows us to create documents
        // that are duplicated X number of times, for many small values of X and
        // a few large values of X.
        for (let i = 0; i < numDocs; i += fibonacci(fibNum++)) {
            let doc = getDoc(i);
            for (let j = 0; j < fibonacci(fibNum); j++) {
                assert.commandWorked(collection.insert(doc));
            }
        }
    }

    // Inserts numDocs into the collection by calling getDoc.
    // NOTE: getDoc is called exactly numDocs times.
    function insertDocumentsUnique(collection, numDocs, getDoc) {
        for (let i = 0; i < numDocs; i++) {
            let doc = getDoc(i);
            assert.commandWorked(collection.insert(doc));
        }
    }

})();
