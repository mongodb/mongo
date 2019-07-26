/**
 * Tests that hybrid index builds are only enabled in FCV 4.2.
 */
(function() {
'use strict';

const dbName = "test";
const collName = "hybrid_indexes";
const dbpath = MongoRunner.dataPath + "hybrid_indexes";

load("jstests/libs/feature_compatibility_version.js");

let conn = MongoRunner.runMongod({binVersion: "latest", cleanData: true, dbpath: dbpath});
let testDB = conn.getDB(dbName);
let testColl = testDB[collName];
testColl.insert({i: 0});
assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

let buildIndex = function(config) {
    const background = config.background;
    const expected = config.expected;

    let res = testDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    assert.commandWorked(res);
    let fcv = res.version;

    clearRawMongoProgramOutput();
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'hangBeforeIndexBuildOf', mode: "alwaysOn", data: {"i": 0}}));

    let awaitBuild;
    if (background) {
        awaitBuild = startParallelShell(function() {
            assert.commandWorked(db.hybrid_indexes.createIndex({i: 1}, {background: true}));
        }, conn.port);
    } else {
        awaitBuild = startParallelShell(function() {
            assert.commandWorked(db.hybrid_indexes.createIndex({i: 1}, {background: false}));
        }, conn.port);
    }

    let msg = "starting on test.hybrid_indexes properties: { v: 2, key: { i: 1.0 }, name: \"i_1\"" +
        ", ns: \"test.hybrid_indexes\", background: " + background + " } using method: " + expected;
    print(msg);
    assert.soon(() => rawMongoProgramOutput().indexOf(msg) >= 0, "Index build not started");
    assert.soon(() => rawMongoProgramOutput().indexOf("Hanging before index build of i=0") >= 0,
                "Index build not hanging");

    if (expected === "Background" || expected === "Hybrid") {
        assert.commandWorked(testColl.insert({i: 1}));
    } else {
        assert.commandFailedWithCode(
            testDB.runCommand({insert: collName, documents: [{i: 2}], maxTimeMS: 100}),
            ErrorCodes.MaxTimeMSExpired);
    }

    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'hangBeforeIndexBuildOf', mode: "off"}));
    awaitBuild();
    assert.commandWorked(testColl.dropIndex("i_1"));
};

// Test: Background indexes behave as background indexes on FCV 4.0.

buildIndex({background: true, expected: "Background"});

// Test: Foreground indexes behave as foreground idnexes on FCV 4.0.

buildIndex({background: false, expected: "Foreground"});

// Test: Upgrade to FCV 4.2 while a background index build is in progress fails. This is subject
// to change, but characterizes the current behavior.

clearRawMongoProgramOutput();
assert.commandWorked(testDB.adminCommand(
    {configureFailPoint: 'hangAfterStartingIndexBuildUnlocked', mode: "alwaysOn"}));

let awaitBuild = startParallelShell(function() {
    // This fails because of the unlock failpoint.
    assert.commandFailedWithCode(db.hybrid_indexes.createIndex({i: 1}, {background: true}),
                                 ErrorCodes.OperationFailed);
}, conn.port);

assert.soon(() => rawMongoProgramOutput().indexOf("Hanging index build with no locks") >= 0,
            "Index build not hanging");

assert.commandFailedWithCode(testDB.adminCommand({setFeatureCompatibilityVersion: "4.2"}),
                             ErrorCodes.BackgroundOperationInProgressForNamespace);

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuildUnlocked', mode: "off"}));
awaitBuild();

// Test: Background indexes behave as hybrid indexes on FCV 4.2.

assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

buildIndex({background: true, expected: "Hybrid"});

// Test: Foreground indexes behave as hybrid indexes on FCV 4.2.

buildIndex({background: false, expected: "Hybrid"});

MongoRunner.stopMongod(conn);
})();
