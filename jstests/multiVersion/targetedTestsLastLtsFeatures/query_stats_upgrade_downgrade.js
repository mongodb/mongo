/**
 * Verifies that $queryStats operates correctly when upgrading and downgrading.
 */

import "jstests/multiVersion/libs/multi_rs.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getQueryStats, verifyMetrics} from "jstests/libs/query_stats_utils.js";
(function() {
"use strict";

function verifyOutput({expectedNShapes, conn}) {
    const queryStats = getQueryStats(conn);
    assert.eq(expectedNShapes, queryStats.length, queryStats);
    verifyMetrics(queryStats);
}

const lastLTSVersion = "last-lts";
const rst = new ReplSetTest(
    {name: jsTestName(), nodes: [{binVersion: lastLTSVersion}, {binVersion: lastLTSVersion}]});
rst.startSet();
rst.initiate();

let testDB = rst.getPrimary().getDB(jsTestName());
let coll = assertDropAndRecreateCollection(testDB, "coll");

assert.commandWorked(coll.insertMany([{x: 0}, {x: 1}, {x: 2}]));

// Check that $queryStats related parameter does not work.
assert.commandFailedWithCode(
    testDB.adminCommand({setParameter: 1, internalQueryStatsRateLimit: -1}),
    ErrorCodes.InvalidOptions);

// Running $queryStats should return an error.
assert.commandFailedWithCode(
    testDB.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}), 40324);

// Run some queries before FCV upgrade. These should not be collected.
let res = coll.aggregate([{$match: {x: 2}}]).toArray();
assert.eq(1, res.length, res);

res = coll.find({x: 2}).toArray();
assert.eq(1, res.length, res);

// Upgrade FCV and check that previous queries were not collected.
rst.upgradeSet({binVersion: "latest"});
assert.commandWorked(rst.getPrimary().getDB("admin").runCommand(
    {setFeatureCompatibilityVersion: latestFCV, confirm: true}));
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB[coll.getName()];

assert.commandWorked(testDB.adminCommand({setParameter: 1, internalQueryStatsRateLimit: -1}));

// The previous queries should not have been collected.
verifyOutput({expectedNShapes: 0, conn: testDB});

// Make new queries and check that they are collected.
res = coll.aggregate([{$match: {x: 2}}, {$sort: {x: -1}}]).toArray();
assert.eq(1, res.length, res);

res = coll.find({x: 2}, {noCursorTimeout: true}).toArray();
assert.eq(1, res.length, res);

verifyOutput(
    {expectedNShapes: 3 /* 1 each for the above, plus $queryStats itself */, conn: testDB});

// Downgrade FCV (without restarting) and check that $queryStats returns an error.
assert.commandWorked(
    testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB[coll.getName()];

// These queries should not be collected.
res = coll.aggregate([{$match: {x: 2}}]).toArray();
assert.eq(1, res.length, res);

res = coll.find({x: 2}).toArray();
assert.eq(1, res.length, res);

// Running $queryStats should return an error.
assert.commandFailedWithCode(
    testDB.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}),
    ErrorCodes.QueryFeatureNotAllowed);

// Upgrade FCV and confirm $queryStats can be run.
assert.commandWorked(rst.getPrimary().getDB("admin").runCommand(
    {setFeatureCompatibilityVersion: latestFCV, confirm: true}));
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB[coll.getName()];

// QueryStats store should not have been cleared. Previous stats collected should still be returned.
// The queries made during the downgrade should not be collected. getQueryStats() uses the same
// query, so the length of 'queryStats' doesn't increase from that.
verifyOutput({expectedNShapes: 3, conn: testDB});

// Fail to downgrade.
jsTestLog("Turning the failpoint on.");
assert.commandWorked(
    rst.getPrimary().adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));
assert.commandFailedWithCode(
    testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}), 549181);
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB[coll.getName()];

// Current FCV is lower than the FCV necessary (7.2) for queryStats to run. These queries should not
// be collected.
res = coll.aggregate([{$match: {x: 2}}]).toArray();
assert.eq(1, res.length, res);

res = coll.find({x: 2}).toArray();
assert.eq(1, res.length, res);

//$queryStats should return an error.
assert.commandFailedWithCode(
    testDB.adminCommand(
        {aggregate: 1, pipeline: [{$queryStats: {}}, {$sort: {key: 1}}], cursor: {}}),
    ErrorCodes.QueryFeatureNotAllowed);

// Successfully upgrade.
assert.commandWorked(rst.getPrimary().getDB("admin").runCommand(
    {setFeatureCompatibilityVersion: latestFCV, confirm: true}));
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB[coll.getName()];

// Stats during the failed downgrade should not have been collected.
verifyOutput({expectedNShapes: 3, conn: testDB});

// New queries should be collected. These queries have a new shape, so we can check the length of
//'queryStats' to confirm they were collected.
res = coll.aggregate([{$match: {x: 2}}], {allowDiskUse: false}).toArray();
assert.eq(1, res.length, res);

res = coll.find({x: 2}, {allowPartialResults: true}).toArray();
assert.eq(1, res.length, res);

verifyOutput({expectedNShapes: 5, conn: testDB});

rst.stopSet();
})();
