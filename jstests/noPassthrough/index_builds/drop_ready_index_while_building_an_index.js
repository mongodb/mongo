/**
 * Tests that the dropIndexes command can drop ready indexes while there are index builds
 * in-progress.
 *
 * @tags: [
 *   # TODO(SERVER-109702): Evaluate if a primary-driven index build compatible test should be created.
 *   requires_commit_quorum,
 *   requires_replication
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 3});
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

const awaitIndexBuild = IndexBuildTest.startIndexBuild(db.getMongo(), coll.getFullName(), {y: 1}, {name: "y_1"});
IndexBuildTest.waitForIndexBuildToScanCollection(db, collName, "y_1");
IndexBuildTest.waitForIndexBuildToScanCollection(secondaryDB, collName, "y_1");

IndexBuildTest.assertIndexes(coll, /*numIndexes=*/ 3, /*readyIndexes=*/ ["_id_", "x_1"], /*notReadyIndexes=*/ ["y_1"]);
// However unlikely, a secondary being in the scan collection phase does not guarantee the oplog
// applier considers the 'startIndexBuild' applied, and as such an attempt to listIndexes might
// fail.
IndexBuildTest.assertIndexesSoon(
    secondaryColl,
    /*numIndexes=*/ 3,
    /*readyIndexes=*/ ["_id_", "x_1"],
    /*notReadyIndexes=*/ ["y_1"],
);

// Drop the ready index while another index build is in-progress.
assert.commandWorked(coll.dropIndex("x_1"));
rst.awaitReplication();

IndexBuildTest.assertIndexes(coll, /*numIndexes=*/ 2, /*readyIndexes=*/ ["_id_"], /*notReadyIndexes=*/ ["y_1"]);
IndexBuildTest.assertIndexes(
    secondaryColl,
    /*numIndexes=*/ 2,
    /*readyIndexes=*/ ["_id_"],
    /*notReadyIndexes=*/ ["y_1"],
);

IndexBuildTest.resumeIndexBuilds(primary);
IndexBuildTest.resumeIndexBuilds(secondary);

awaitIndexBuild();
rst.awaitReplication();

IndexBuildTest.assertIndexes(coll, /*numIndexes=*/ 2, /*readyIndexes=*/ ["_id_", "y_1"], /*notReadyIndexes=*/ []);
IndexBuildTest.assertIndexes(
    secondaryColl,
    /*numIndexes=*/ 2,
    /*readyIndexes=*/ ["_id_", "y_1"],
    /*notReadyIndexes=*/ [],
);

rst.stopSet();
