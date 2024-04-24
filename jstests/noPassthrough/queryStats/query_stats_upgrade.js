/**
 * Test that query stats doesn't work on a lower FCV version but works after an FCV upgrade.
 * @tags: [featureFlagQueryStats]
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
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: binVersionToFCV("last-lts")}));
    if (restart) {
        MongoRunner.stopMongod(conn);
        conn = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
        testDB = conn.getDB(jsTestName());
        adminDB = conn.getDB("admin");
    }

    assert.commandFailedWithCode(
        testDB.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}),
        ErrorCodes.QueryFeatureNotAllowed);

    // Upgrade FCV.
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: binVersionToFCV("latest")}));

    // We should be able to run a query stats pipeline now that the FCV is correct.
    assert.commandWorked(
        testDB.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}));
}
testLower(true);
testLower(false);
MongoRunner.stopMongod(conn);
})();