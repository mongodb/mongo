/**
 * Test that query stats works both before and after an FCV upgrade.
 * @tags: [
 *   # Query Stats should not skip FCV gating before 7.0.
 *   requires_fcv_70,
 *   # Re-uses FCV state in the dbpath.
 *   requires_persistence
 *  ]
 */
load('jstests/libs/analyze_plan.js');
load("jstests/libs/feature_flag_util.js");

(function() {
"use strict";

const dbpath = MongoRunner.dataPath + jsTestName();
let conn = MongoRunner.runMongod({dbpath: dbpath});
let testDB = conn.getDB(jsTestName());

function testLower(restart = false) {
    let adminDB = conn.getDB("admin");
    assert.commandWorked(adminDB.runCommand(
        {setFeatureCompatibilityVersion: binVersionToFCV("last-lts"), confirm: true}));
    if (restart) {
        MongoRunner.stopMongod(conn);
        conn = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
        testDB = conn.getDB(jsTestName());
        adminDB = conn.getDB("admin");
    }

    // We should be able to run a query stats pipeline even though the FCV is not upgraded.
    assert.commandWorked(
        testDB.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}));

    // Upgrade FCV.
    assert.commandWorked(adminDB.runCommand(
        {setFeatureCompatibilityVersion: binVersionToFCV("latest"), confirm: true}));

    // We should still be able to run a query stats pipeline on the upgraded FCV.
    assert.commandWorked(
        testDB.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}),
    );
}
testLower(true);
testLower(false);
MongoRunner.stopMongod(conn);
})();
