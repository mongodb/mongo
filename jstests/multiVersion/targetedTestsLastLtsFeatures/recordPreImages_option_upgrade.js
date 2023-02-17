/**
 * Verifies that the server ignores collection option "recordPreImages" on binary upgrade from the
 * last LTS version to the current, as well as removes the option from collection attributes on
 * FCV upgrade.
 */
(function() {
"use strict";
load('jstests/multiVersion/libs/multi_rs.js');

const lastLTSVersion = "last-lts";
const latestVersion = "latest";

// Setup a two-node replica set with last LTS binaries, so it is possible to create a collection
// with "recordPreImages" option.
const rst = new ReplSetTest(
    {name: jsTestName(), nodes: [{binVersion: lastLTSVersion}, {binVersion: lastLTSVersion}]});
rst.startSet();
rst.initiate();
const testDB = rst.getPrimary().getDB("test");
const primaryNode = rst.getPrimary();
const secondaryNode = rst.getSecondary();

// Create the collection.
const collectionName = "coll";
assert.commandWorked(testDB.createCollection(collectionName, {recordPreImages: true}));
let coll = testDB[collectionName];

// Insert a test document which will be updated to trigger recording of change stream pre-images.
assert.commandWorked(coll.insert({_id: 1, a: 1}));
assert.commandWorked(coll.updateOne({_id: 1}, {$inc: {a: 1}}));
rst.awaitReplication();

// Upgrade the binary of the secondary node to the current version to setup a mixed binary cluster.
rst.upgradeMembers([secondaryNode], {binVersion: latestVersion});

// Make sure the primary node did not change.
rst.stepUp(primaryNode);

// Verify that recording of change stream pre-images succeeds.
assert.commandWorked(coll.updateOne({_id: 1}, {$inc: {a: 1}}));
rst.awaitReplication();

// Finally upgrade the binary of the primary node to the current version.
rst.upgradePrimary(rst.getPrimary(), {binVersion: latestVersion});

// Update a document on the collection with inactive "recordPreImages" collection option.
coll = rst.getPrimary().getDB("test")[collectionName];
assert.commandWorked(coll.updateOne({_id: 1}, {$inc: {a: 1}}));
rst.awaitReplication();

// Upgrade the FCV to the latest to trigger removal of "recordPreImages" collection option from
// persistent catalog entries.
assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// To check the collection options, downgrade FCV to later replace the binary of the server with
// the last LTS binary version.
assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
rst.upgradeSet({binVersion: lastLTSVersion});

// Verify that collection option "recordPreImages" was removed.
const result =
    assert.commandWorked(rst.getPrimary().getDB("test").runCommand({listCollections: 1}));
assert.eq(result.cursor.firstBatch[0].name, collectionName);
assert.docEq(
    {},
    result.cursor.firstBatch[0].options,
    `Collection option "recordPreImages" was not removed. Got response: ${tojson(result)}`);
rst.stopSet();
})();