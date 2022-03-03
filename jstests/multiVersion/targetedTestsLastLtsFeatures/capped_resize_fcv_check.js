/**
 * Tests that resizing capped collections fails when downgrading from a FCV version 6.0 or higher to
 * a version below 6.0.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 2});
const nodes = rst.startSet();
rst.initiate();

const maxSize = 25 * 1024 * 1024;  // 25 MB.
const maxDocs = 2;
const doubleMaxSize = 50 * 1024 * 1024;  // 50 MB.
const doubleMaxDocs = 4;

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const cappedColl = testDB["capped_coll"];
cappedColl.drop();
assert.commandWorked(
    testDB.createCollection(cappedColl.getName(), {capped: true, size: maxSize, max: maxDocs}));

// On version 6.0, we expect the collMod command to succeed.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(testDB.runCommand(
    {collMod: cappedColl.getName(), cappedSize: doubleMaxSize, cappedMax: doubleMaxDocs}));
let stats = assert.commandWorked(cappedColl.stats());
assert.eq(stats.maxSize, doubleMaxSize);
assert.eq(stats.max, doubleMaxDocs);

// On versions <6.0, we expect the command to fail and the capped collection size limits to remain
// the same.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));
assert.commandFailed(
    testDB.runCommand({collMod: cappedColl.getName(), cappedSize: maxSize, cappedMax: maxDocs}));
stats = assert.commandWorked(cappedColl.stats());
assert.eq(stats.maxSize, doubleMaxSize);
assert.eq(stats.max, doubleMaxDocs);

// Upgrade and resize the capped collection.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "6.0"}));
assert.commandWorked(
    testDB.runCommand({collMod: cappedColl.getName(), cappedSize: maxSize, cappedMax: maxDocs}));
stats = assert.commandWorked(cappedColl.stats());
assert.eq(stats.maxSize, maxSize);
assert.eq(stats.max, maxDocs);

// We expect the resizing command to fail and the size limits to remain the same.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
assert.commandFailed(testDB.runCommand(
    {collMod: cappedColl.getName(), cappedSize: doubleMaxSize, cappedMax: doubleMaxDocs}));
stats = assert.commandWorked(cappedColl.stats());
assert.eq(stats.maxSize, maxSize);
assert.eq(stats.max, maxDocs);

rst.stopSet();
})();
