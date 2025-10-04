/**
 * Tests that resumable index build state is written to disk upon clean shutdown when an index
 * build is in the drain writes phase on a secondary, and that the index build is subsequently
 * completed when the node is started back up.
 *
 * For this test, the secondary is ineligible to become primary, but still has a vote so it is
 * required for the commit quorum to be satisfied.
 *
 * @tags: [
 *   # Primary-driven index builds aren't resumable.
 *   # This test uses a failpoint that is only available for hybrid index builds.
 *   primary_driven_index_builds_incompatible,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const dbName = "test";
const collName = jsTestName();

let rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}],
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let coll = primary.getDB(dbName).getCollection(collName);

assert.commandWorked(coll.insert({a: 1}));

jsTestLog("Testing when secondary shuts down in the middle of the first drain");

ResumableIndexBuildTest.runOnSecondary(
    rst,
    dbName,
    collName,
    {a: 1},
    "hangIndexBuildDuringDrainWritesPhase",
    0,
    undefined /* primaryFailPointName */,
    [{a: 2}, {a: 3}],
    [{a: 4}, {a: 5}],
);
ResumableIndexBuildTest.runOnSecondary(
    rst,
    dbName,
    collName,
    {a: 1},
    "hangIndexBuildDuringDrainWritesPhase",
    1,
    undefined /* primaryFailPointName */,
    [{a: 6}, {a: 7}],
    [{a: 8}, {a: 9}],
);

jsTestLog("Testing when secondary shuts down before voting");

ResumableIndexBuildTest.runOnSecondary(
    rst,
    dbName,
    collName,
    {a: 1},
    "hangAfterIndexBuildFirstDrain",
    {},
    undefined /* primaryFailPointName */,
    [{a: 10}, {a: 11}],
    [{a: 12}, {a: 13}],
);

jsTestLog(
    "Testing when secondary shuts down after commit quorum satisfied, but before replicating commitIndexBuild oplog entry",
);

ResumableIndexBuildTest.runOnSecondary(
    rst,
    dbName,
    collName,
    {a: 1},
    "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
    {},
    "hangIndexBuildBeforeCommit",
    [{a: 14}, {a: 15}],
    [{a: 16}, {a: 17}],
);

rst.stopSet();
