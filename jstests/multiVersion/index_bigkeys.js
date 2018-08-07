/**
 * Test interactions with big keys in different versions.
 * TODO SERVER-36280: Add testcases for feature tracker bit for overlong TypeBits.
 * TODO SERVER-36385: Remove this test in 4.4
 */

"use strict";

const dbName = "test";
const collName = "index_bigkeys";

const largeKey = 's'.repeat(12345);
const documentWithLargeKey = {
    x: largeKey
};

function testInsertDocumentWithLargeKey(conn, expectKeyTooLongFailure) {
    let testDB = conn.getDB(dbName);
    let testColl = testDB[collName];

    testColl.drop();
    assert.commandWorked(
        testDB.runCommand({createIndexes: collName, indexes: [{key: {x: 1}, name: "x_1"}]}));

    if (expectKeyTooLongFailure) {
        assert.commandFailedWithCode(
            testDB.runCommand({insert: collName, documents: [documentWithLargeKey]}),
            ErrorCodes.KeyTooLong);
        assert.eq(0, testColl.count());
    } else {
        assert.commandWorked(
            testDB.runCommand({insert: collName, documents: [documentWithLargeKey]}));
        assert.eq(1, testColl.count());
    }
}

function downgradeAndVerifyBehavior(testDowngradeBehaviorFunc) {
    const dbpath = MongoRunner.dataPath + "index_bigkeys";

    // Test setting FCV4.0.
    let conn = MongoRunner.runMongod({binVersion: "latest", cleanData: true, dbpath: dbpath});
    let testDB = conn.getDB(dbName);
    let testColl = testDB[collName];
    assert.commandWorked(
        testDB.runCommand({createIndexes: collName, indexes: [{key: {x: 1}, name: "x_1"}]}));
    assert.commandWorked(testDB.runCommand({insert: collName, documents: [documentWithLargeKey]}));
    assert.eq(1, testColl.count());
    assert.eq(2, testColl.getIndexes().length);

    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    testDowngradeBehaviorFunc(testDB, testColl);
    MongoRunner.stopMongod(conn, null, {skipValidation: true});

    // Test downgrading to 4.0 binary.
    conn = MongoRunner.runMongod({binVersion: "latest", cleanData: true, dbpath: dbpath});
    testDB = conn.getDB(dbName);
    testColl = testDB[collName];
    assert.commandWorked(
        testDB.runCommand({createIndexes: collName, indexes: [{key: {x: 1}, name: "x_1"}]}));
    assert.commandWorked(testDB.runCommand({insert: collName, documents: [documentWithLargeKey]}));
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    MongoRunner.stopMongod(conn, null, {skipValidation: true});

    conn = MongoRunner.runMongod({binVersion: "4.0", noCleanData: true, dbpath: dbpath});
    testDowngradeBehaviorFunc(conn.getDB(dbName), conn.getDB(dbName)[collName]);
    MongoRunner.stopMongod(conn, null, {skipValidation: false});
}

function upgradeAndVerifyBehavior(testUpgradeBehaviorFunc) {
    const dbpath = MongoRunner.dataPath + "index_bigkeys";

    // Test upgrading to 4.2 binary.
    let conn = MongoRunner.runMongod({binVersion: "4.0", cleanData: true, dbpath: dbpath});
    let testDB = conn.getDB(dbName);
    let testColl = testDB[collName];
    assert.commandWorked(
        testDB.runCommand({createIndexes: collName, indexes: [{key: {x: 1}, name: "x_1"}]}));
    assert.commandFailedWithCode(
        testDB.runCommand({insert: collName, documents: [documentWithLargeKey]}),
        ErrorCodes.KeyTooLong);
    assert.eq(0, testColl.count());

    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.0"}));
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({binVersion: "latest", noCleanData: true, dbpath: dbpath});
    testDB = conn.getDB(dbName);
    testColl = testDB[collName];
    assert.commandFailedWithCode(
        testDB.runCommand({insert: collName, documents: [documentWithLargeKey]}),
        ErrorCodes.KeyTooLong);
    assert.eq(0, testColl.count());

    // Setting the FCV to 4.2
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    testUpgradeBehaviorFunc(testDB, testColl);
    MongoRunner.stopMongod(conn, null, {skipValidation: false});
}

(function() {
    load("jstests/libs/feature_compatibility_version.js");

    // 1. Test the behavior of inserting large index key of each version.
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

    // 2. Test that 4.0 binary could query with large keys and remove docs with large keys which got
    // inserted by 4.2 binary.
    downgradeAndVerifyBehavior((testDB, testColl) => {
        assert.commandWorked(
            testDB.runCommand({delete: collName, deletes: [{q: {x: largeKey}, limit: 0}]}));
        assert.eq(0, testColl.count());
    });

    // 3. Test that 4.0 binary could update large keys with short keys.
    downgradeAndVerifyBehavior((testDB, testColl) => {
        assert.commandWorked(
            testDB.runCommand({update: collName, updates: [{q: {x: largeKey}, u: {x: "sss"}}]}));
        assert.eq("sss", testColl.find({x: "sss"}).toArray()[0].x);
    });

    // 4. Test that 4.0 binary could drop the index which has large keys.
    downgradeAndVerifyBehavior((testDB, testColl) => {
        assert.eq(2, testColl.getIndexes().length);
        assert.commandWorked(testDB.runCommand({dropIndexes: collName, index: "x_1"}));
        assert.eq(1, testColl.getIndexes().length);
    });

    // 5. Test the normal upgrade path.
    upgradeAndVerifyBehavior((testDB, testColl) => {
        assert.commandWorked(
            testDB.runCommand({insert: collName, documents: [documentWithLargeKey]}));
    });
}());
