/**
 * Regression test that ensure there have been no KeyString encoding changes between last-stable the
 * current version. Has the following procedure:
 * - Start mongod with the last-stable version.
 * - For each index type, create a new collection, insert documents, and create an index.
 * - Shutdown mongod and restart with the latest version.
 * - Run validate.
 * - Drop all collections.
 * - Recreate all indexes.
 * - Shuwdown mongod and restart with the last-stable version.
 * - Run validate.
 *
 * The following index types are tested:
 * - btree
 * - 2d
 * - geoHaystack
 * - 2dsphere
 * - text
 * - hashed*
 * - wildcard*
 * * These indexes are only created as v2 and non-unique because v1 does not support these features.
 *
 * For each index type, a v1 unique, v2 unique, v1 non-unique and v2 non-unique index
 * is considered except for hashed and wildcard, which only consider the v2 non-unique case.
 */
(function() {
'use strict';
load('jstests/hooks/validate_collections.js');

const kNumDocs = 100;

const indexTypes = [
    {
        // This is the name of the index.
        indexName: "BTreeIndex",
        // This function is called to create documents, which are then inserted into the collection.
        createDoc: i => ({
            a: i,
            b: {x: i, y: i + 1},
            c: [i, i + 1],
        }),
        // This is the index key specification.
        spec: {a: 1, b: 1, c: -1},
        // This optional parameter specifies extra options to give to the createIndex helper.
        // e.g. collection.createIndexes(spec, createIndexOptions)
        createIndexOptions: {},
    },
    {
        indexName: "2d",
        createDoc: i => ({loc: [i, i]}),
        spec: {loc: "2d"},
    },
    {
        indexName: "hayStack",
        createDoc: i => ({
            loc: {lng: (i / 2.0) * (i / 2.0), lat: (i / 2.0)},
            a: {x: i, y: i + 1, z: [i, i + 1]},
        }),
        spec: {loc: "geoHaystack", a: 1},
        createIndexOptions: {bucketSize: 1},
    },
    {
        indexName: "2dSphere",
        createDoc: i => {
            if (i == 0)
                return {
                    "loc": {
                        "type": "Polygon",
                        "coordinates": [[[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]]
                    },
                    b: {x: i, y: i + 1},
                    c: [i, i + 1],
                };
            else
                return ({
                    loc: {type: "Point", coordinates: [(i / 10.0) * (i / 10.0), (i / 10.0)]},
                    b: {x: i, y: i + 1},
                    c: [i, i + 1],
                });
        },
        spec: {loc: "2dsphere", b: 1, c: -1},
    },
    {
        indexName: "text",
        createDoc: i => ({
            a: "a".repeat(i + 1),
            b: {x: i, y: i + 1, z: [i, i + 1]},
        }),
        spec: {a: "text", b: 1},
    },
    {
        indexName: "hashed",
        createDoc: i => ({
            a: {x: i, y: i + 1, z: [i, i + 1]},
        }),
        spec: {a: "hashed"},
    },
    {
        indexName: "wildCard",
        createDoc: i => {
            if (i == 0)
                return {};
            else if (i == 1)
                return {a: null};
            else if (i == 2)
                return {a: {}};
            else
                return {
                    a: i,
                    b: {x: i, y: i + 1},
                    c: [i, i + 1],
                };
        },
        spec: {"$**": 1},
    }
];
// -----

const dbpath = MongoRunner.dataPath + 'keystring_index';
resetDbpath(dbpath);

const defaultOptions = {
    dbpath: dbpath,
    noCleanData: true
};

const kCollectionPrefix = 'testColl';

let mongodOptionsLastStable = Object.extend({binVersion: 'last-stable'}, defaultOptions);
let mongodOptionsCurrent = Object.extend({binVersion: 'latest'}, defaultOptions);

// We will first start up a last-stable version mongod, populate the database, upgrade, then
// validate.

jsTestLog("Starting version: last-stable");
let conn = MongoRunner.runMongod(mongodOptionsLastStable);

assert.neq(
    null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptionsLastStable));

let testDb = conn.getDB('test');

populateDb(testDb);
MongoRunner.stopMongod(conn);

jsTestLog("Starting version: latest");

// Restart the mongod with the latest binary version on the old version's data files.
conn = MongoRunner.runMongod(mongodOptionsCurrent);
assert.neq(null, conn, 'mongod was unable to start with the latest version');
testDb = conn.getDB('test');
assert.gt(testDb.getCollectionInfos().length, 0);

jsTestLog(
    "Validating indexes created with a 'last-stable' version binary using a 'latest' version " +
    "binary");

// Validate all the indexes.
assert.commandWorked(validateCollections(testDb, {full: true}));

// Next, we will repopulate the database with the latest version then downgrade and run validate.
dropAllUserCollections(testDb);
populateDb(testDb);
MongoRunner.stopMongod(conn);

conn = MongoRunner.runMongod(mongodOptionsLastStable);
assert.neq(
    null, conn, 'mongod was unable able to start with version ' + tojson(mongodOptionsLastStable));

testDb = conn.getDB('test');
assert.gt(testDb.getCollectionInfos().length, 0);

jsTestLog(
    "Validating indexes created with 'latest' version binary using a 'last-stable' version binary");

assert.commandWorked(validateCollections(testDb, {full: true}));
MongoRunner.stopMongod(conn);

// Populate the database using the config specified by the indexTypes array.
function populateDb(testDb) {
    // Create a new collection and index for each indexType in the array.
    indexTypes.forEach(indexOptions => {
        // Try unique and non-unique.
        [true, false].forEach(unique => {
            // Try index-version 1 and 2.
            [1, 2].forEach(indexVersion => {
                let indexName = indexOptions.indexName;

                // We only run V2 non-unique for hashed and wildCard because they don't exist in
                // v1.
                if ((indexName == "hashed" || indexName == "wildCard") &&
                    (unique === true || indexVersion === 1))
                    return;

                indexName += unique === true ? "Unique" : "NotUnique";
                indexName += `Version${indexVersion}`;
                let collectionName = kCollectionPrefix + '_' + indexName;
                print(`${indexName}: Creating Collection`);
                assert.commandWorked(testDb.createCollection(collectionName));

                print(`${indexName}: Inserting Documents`);
                if (unique)
                    insertDocumentsUnique(testDb[collectionName], kNumDocs, indexOptions.createDoc);
                else
                    insertDocumentsNotUnique(
                        testDb[collectionName], kNumDocs, indexOptions.createDoc);

                let extraCreateIndexOptions = {
                    name: indexName,
                    v: NumberLong(indexVersion),
                    unique: unique === true
                };

                if ("createIndexOptions" in indexOptions)
                    extraCreateIndexOptions =
                        Object.extend(extraCreateIndexOptions, indexOptions.createIndexOptions);
                print(JSON.stringify(extraCreateIndexOptions));
                print(`${indexName}: Creating Index`);
                assert.commandWorked(
                    testDb[collectionName].createIndex(indexOptions.spec, extraCreateIndexOptions));

                // Assert that the correct index type was created.
                let indexSpec = getIndexSpecByName(testDb[collectionName], indexName);
                assert.eq(indexVersion, indexSpec.v, tojson(indexSpec));
            });
        });
    });
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
    assert.eq(
        1, indexesFilteredByName.length, "index '" + indexName + "' not found: " + tojson(indexes));
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
    var bulk = collection.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i += fibonacci(fibNum++)) {
        let doc = getDoc(i);
        for (let j = 0; j < fibonacci(fibNum); j++) {
            bulk.insert(doc);
        }
    }
    assert.commandWorked(bulk.execute());
}

// Inserts numDocs into the collection by calling getDoc.
// NOTE: getDoc is called exactly numDocs times.
function insertDocumentsUnique(collection, numDocs, getDoc) {
    var bulk = collection.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        let doc = getDoc(i);
        bulk.insert(doc);
    }
    assert.commandWorked(bulk.execute());
}
})();
