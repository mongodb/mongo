/**
 * Tests that a primary-driven index build (PDIB) succeeds when WriteConflictExceptions are injected
 * during the drain phase, when side writes captured concurrently with the build are applied to the
 * actual index table (side_writes_tracker.cpp drain path and multi_index_block.cpp insertion
 * batches).
 *
 * Pattern:
 *   - Pre-populate a small base corpus so the build starts quickly.
 *   - Pause the build at the start of the collection scan so side writes accumulate.
 *   - Drive a substantial volume of concurrent writes against the collection while the build is
 *     paused — these land in the side writes table.
 *   - Arm WTWriteConflictException with a bounded `times` and resume the build. The conflicts fire
 *     during the drain (and replicated index inserts on the secondary), and the writeConflictRetry
 *     loops on the drain path must absorb them.
 *
 * Companion to SERVER-118892 / SERVER-122301 covering the *index*-table write path (the spill /
 * merge tests cover the *sorter*-table write path).
 *
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "test";
const collName = jsTestName();
const db = primary.getDB(dbName);
const coll = db.getCollection(collName);
const indexName = "x_1";
const indexSpec = {x: 1};

// TODO(SERVER-109578): Remove this check when the feature flag is removed.
if (!FeatureFlagUtil.isPresentAndEnabled(db, "PrimaryDrivenIndexBuilds")) {
    jsTest.log.info("Skipping test because featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

// TODO(SERVER-109578): Remove this check when the feature flag is removed.
if (!FeatureFlagUtil.isPresentAndEnabled(db, "ContainerWrites")) {
    jsTest.log.info("Skipping test because featureFlagContainerWrites is disabled");
    rst.stopSet();
    quit();
}

// Seed the collection so the build has scan work but completes quickly once resumed.
const seed = 5000;
let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < seed; i++) {
    bulk.insert({_id: i, x: i});
}
assert.commandWorked(bulk.execute());
rst.awaitReplication();

// Pause the build so concurrent writes accumulate in the side writes tracker rather than being
// drained immediately.
IndexBuildTest.pauseIndexBuilds(primary);

const awaitIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), indexSpec, {name: indexName});
IndexBuildTest.waitForIndexBuildToStart(db, collName, indexName);

// While the build is paused, drive concurrent writes — these become side writes that the drain
// phase must replay against the actual index table.
const sideWrites = 2000;
bulk = coll.initializeUnorderedBulkOp();
for (let i = seed; i < seed + sideWrites; i++) {
    bulk.insert({_id: i, x: i});
}
assert.commandWorked(bulk.execute());

// Also update a chunk of the base corpus to exercise the side-writes update path (delete + insert
// pair on the index table during drain).
const updates = 500;
for (let i = 0; i < updates; i++) {
    assert.commandWorked(coll.update({_id: i}, {$set: {x: -i}}));
}

// Now arm the failpoint and let the build (and its drain phase) run.
const writeConflicts = 30;
const fp = configureFailPoint(primary, "WTWriteConflictException", {}, {times: writeConflicts});

IndexBuildTest.resumeIndexBuilds(primary);
awaitIndex();

fp.off();

// The build must have completed and produced an index containing every base + side-write record.
IndexBuildTest.assertIndexes(coll, /*numIndexes=*/ 2, [indexName], [], {includeBuildUUIDs: false});

rst.awaitReplication();
const secondary = rst.getSecondary();
IndexBuildTest.assertIndexes(
    secondary.getDB(dbName).getCollection(collName),
    /*numIndexes=*/ 2,
    [indexName],
    [],
    {includeBuildUUIDs: false},
);

// Index scan must return every row that's currently in the collection.
const expectedCount = seed + sideWrites;
assert.eq(coll.find({}).hint({x: 1}).itcount(), expectedCount);

rst.stopSet();
