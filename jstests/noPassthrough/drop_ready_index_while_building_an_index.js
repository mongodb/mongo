/**
 * Tests that the dropIndexes command can drop ready indexes while there are index builds
 * in-progress.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const rst = ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

const dbName = "test";
const collName = jsTestName();

const db = primary.getDB(dbName);
const coll = db.getCollection(collName);
const secondaryDB = secondary.getDB(dbName);
const secondaryColl = secondaryDB.getCollection(collName);

assert.commandWorked(db.createCollection(collName));

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({x: i, y: i}));
}

assert.commandWorked(coll.createIndex({x: 1}, {name: "x_1"}));

IndexBuildTest.pauseIndexBuilds(primary);
IndexBuildTest.pauseIndexBuilds(secondary);

const awaitIndexBuild =
    IndexBuildTest.startIndexBuild(db.getMongo(), coll.getFullName(), {y: 1}, {name: "y_1"});
IndexBuildTest.waitForIndexBuildToScanCollection(db, collName, "y_1");
IndexBuildTest.waitForIndexBuildToScanCollection(secondaryDB, collName, "y_1");

IndexBuildTest.assertIndexes(
    coll, /*numIndexes=*/3, /*readyIndexes=*/["_id_", "x_1"], /*notReadyIndexes=*/["y_1"]);
IndexBuildTest.assertIndexes(
    secondaryColl, /*numIndexes=*/3, /*readyIndexes=*/["_id_", "x_1"], /*notReadyIndexes=*/["y_1"]);

// Drop the ready index while another index build is in-progress.
assert.commandWorked(coll.dropIndex("x_1"));
rst.awaitReplication();

IndexBuildTest.assertIndexes(
    coll, /*numIndexes=*/2, /*readyIndexes=*/["_id_"], /*notReadyIndexes=*/["y_1"]);
IndexBuildTest.assertIndexes(
    secondaryColl, /*numIndexes=*/2, /*readyIndexes=*/["_id_"], /*notReadyIndexes=*/["y_1"]);

IndexBuildTest.resumeIndexBuilds(primary);
IndexBuildTest.resumeIndexBuilds(secondary);

awaitIndexBuild();
rst.awaitReplication();

IndexBuildTest.assertIndexes(
    coll, /*numIndexes=*/2, /*readyIndexes=*/["_id_", "y_1"], /*notReadyIndexes=*/[]);
IndexBuildTest.assertIndexes(
    secondaryColl, /*numIndexes=*/2, /*readyIndexes=*/["_id_", "y_1"], /*notReadyIndexes=*/[]);

rst.stopSet();
}());
