/**
 * Test interactions with big keys in different versions:
 * 1. MongoDB 4.2 (FCV 4.0) and MongoDB 4.0 allow reading and deleting long (unique) index keys but
 * does not allow inserting or updating long (unique) index keys.
 * 2. MongoDB 4.2 (FCV 4.2) allows inserting, reading, deleting and updating long (unique) index
 * keys.
 * 3. MongoDB 4.2 could successfully validate index consistency after reindex() if 4.0 intentionally
 * missed out long index keys by setting 'failIndexKeyTooLong' to false.
 * 4. MongoDB 4.0 could successfully validate index consistency after drop() if 4.2 inserted long
 * index keys (with < 127 bytes typebits).
 *
 * TODO SERVER-36385: Remove this test in 4.4
 */

"use strict";

const dbName = "test";
const collName = "index_bigkeys";
const dbpath = MongoRunner.dataPath + "index_bigkeys";

const largeKey = 's'.repeat(12345);
const documentWithLargeKey = {
    x: largeKey
};

/**
 * Assert that number of index keys on 'x' is equal to the collection count.
 */
function assertAllThingsAreIndexed(testColl) {
    assert.eq(testColl.find().sort({x: 1}).itcount(), testColl.count());
}

function testInsertDocumentWithLargeKey(conn, expectKeyTooLongFailure) {
    let testDB = conn.getDB(dbName);
    let testColl = testDB[collName];

    [true, false].forEach(function(buildIndexFirst) {
        [true, false].forEach(function(backgroundIndexBuild) {
            [true, false].forEach(function(uniqueIndex) {
                testColl.drop();
                let res;

                if (buildIndexFirst) {
                    assert.commandWorked(testColl.createIndex(
                        {x: 1},
                        {name: "x_1", background: backgroundIndexBuild, unique: uniqueIndex}));
                    res = testColl.insert(documentWithLargeKey);
                } else {
                    testColl.insert(documentWithLargeKey);
                    res = testColl.createIndex(
                        {x: 1},
                        {name: "x_1", background: backgroundIndexBuild, unique: uniqueIndex});
                }

                if (expectKeyTooLongFailure)
                    assert.commandFailedWithCode(res, ErrorCodes.KeyTooLong);
                else
                    assert.commandWorked(res);

                assertAllThingsAreIndexed(testColl);
            });
        });
    });
}

/**
 * Insert big index keys in FCV 4.2 and downgrade all the way to 4.0 binary and
 * verify the behaviors of FCV 4.0 and 4.0 binary.
 */
function downgradeAndVerifyBehavior(testDowngradeBehaviorFunc) {
    // Only test non-unique index for downgrade path because 4.2 and 4.0 have different formats of
    // unique index.
    let uniqueIndex = false;

    // 1. Downgrade to FCV 4.0 and verify behaviors.
    let conn = MongoRunner.runMongod({binVersion: "latest", cleanData: true, dbpath: dbpath});
    let testColl = conn.getDB(dbName)[collName];
    assert.commandWorked(testColl.createIndex({x: 1}, {name: "x_1", unique: uniqueIndex}));
    assert.commandWorked(testColl.insert(documentWithLargeKey));
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    assertAllThingsAreIndexed(testColl);
    testDowngradeBehaviorFunc(testColl);
    // We skip the validation because FCV 4.0 cannot validate a big index key
    // inserted by FCV 4.2.
    MongoRunner.stopMongod(conn, null, {skipValidation: true});

    // 2. Start fresh on a 4.2 binary and insert long index keys. Then downgrade to FCV 4.0 followed
    // by restarting with a 4.0 binary and verify behaviors.
    conn = MongoRunner.runMongod({binVersion: "latest", cleanData: true, dbpath: dbpath});
    testColl = conn.getDB(dbName)[collName];
    assert.commandWorked(testColl.createIndex({x: 1}, {name: "x_1", unique: uniqueIndex}));
    assert.commandWorked(testColl.insert(documentWithLargeKey));
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    // We skip the validation because FCV 4.0 cannot validate a big index key
    // inserted by FCV 4.2.
    MongoRunner.stopMongod(conn, null, {skipValidation: true});
    conn = MongoRunner.runMongod({binVersion: "4.0", noCleanData: true, dbpath: dbpath});
    testColl = conn.getDB(dbName)[collName];
    assertAllThingsAreIndexed(testColl);
    testDowngradeBehaviorFunc(testColl);
    MongoRunner.stopMongod(conn, null, {skipValidation: true});
}

(function() {
load("jstests/libs/feature_compatibility_version.js");

// Test the behavior of inserting big index keys of each version.
// 4.2 binary (with FCV 4.2)
let conn = MongoRunner.runMongod({binVersion: "latest", cleanData: true});
testInsertDocumentWithLargeKey(conn, false);

// 4.2 binary (with FCV 4.0)
assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
testInsertDocumentWithLargeKey(conn, true);
MongoRunner.stopMongod(conn);

// 4.0 binary
conn = MongoRunner.runMongod({binVersion: "4.0", cleanData: true});
testInsertDocumentWithLargeKey(conn, true);
MongoRunner.stopMongod(conn);

// Downgrade path
// 1. Test that 4.0 binary could read and delete big index keys which got
// inserted by 4.2 binary.
downgradeAndVerifyBehavior(testColl => {
    assert.commandWorked(testColl.remove({x: largeKey}));
    assert(testColl.validate().valid);
});

// 2. Test that 4.0 binary could update big keys with small keys.
downgradeAndVerifyBehavior(testColl => {
    assert.commandWorked(testColl.update({x: largeKey}, {$set: {x: "sss"}}));
    assert.eq("sss", testColl.find({x: "sss"}).toArray()[0].x);
});

// 3. Test that 4.0 binary could drop the index which has big keys and the
// validate will succeed after that.
downgradeAndVerifyBehavior(testColl => {
    assert.eq(2, testColl.getIndexes().length);
    assert(!testColl.validate().valid);
    assert.commandWorked(testColl.dropIndex({x: 1}));
    assert.eq(1, testColl.getIndexes().length);
    assert(testColl.validate().valid);
});

// Upgrade path
// 1. Test the normal upgrade path.
[true, false].forEach(function(uniqueIndex) {
    // Upgrade all the way to 4.2 binary with FCV 4.2.
    let conn = MongoRunner.runMongod({binVersion: "4.0", cleanData: true, dbpath: dbpath});
    assert.commandWorked(
        conn.getDB(dbName)[collName].createIndex({x: 1}, {name: "x_1", unique: uniqueIndex}));
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({binVersion: "latest", noCleanData: true, dbpath: dbpath});
    // Setting the FCV to 4.2
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    assert.commandWorked(
        conn.getDB(dbName).runCommand({insert: collName, documents: [documentWithLargeKey]}));
    MongoRunner.stopMongod(conn, null, {skipValidation: false});
});

// 2. If 4.0 binary has already inserted documents with large keys by setting
// 'failIndexKeyTooLong' to be false (which bypasses inserting the index key), 4.2 binary cannot
// successfully validate the index consistency because some index keys are missing. But reindex
// should solve this problem.
[true, false].forEach(function(uniqueIndex) {
    let conn = MongoRunner.runMongod({
        binVersion: "4.0",
        cleanData: true,
        setParameter: "failIndexKeyTooLong=false",
        dbpath: dbpath
    });
    assert.commandWorked(
        conn.getDB(dbName)[collName].createIndex({x: 1}, {name: "x_1", unique: uniqueIndex}));
    assert.commandWorked(conn.getDB(dbName)[collName].insert(documentWithLargeKey));
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({binVersion: "latest", noCleanData: true, dbpath: dbpath});
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    let testColl = conn.getDB(dbName)[collName];
    assert(!testColl.validate().valid);
    testColl.reIndex();
    assert(testColl.validate().valid);

    MongoRunner.stopMongod(conn, null, {skipValidation: false});
});
}());
