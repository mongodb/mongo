/**
 * When an index build on a secondary fails, we expect to receive a abortIndexBuild oplog entry from
 * the primary eventually. If we get a commitIndexBuild oplog entry instead, the secondary should
 * crash.
 * @tags: [
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

// Because this test intentionally crashes the server via an fassert, we need to instruct the
// shell to clean up the core dump that is left behind.
TestData.cleanUpCoreDumpsFromExpectedCrash = true;

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ],
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const testDB = primary.getDB("test");
const coll = testDB.getCollection("test");

assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(primary);

// Make the index build fail on the secondary during the collection scan phase.
// When we unblock the index build on the primary, the index build will complete successfully.
const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
assert.commandWorked(
    secondaryDB.adminCommand({configureFailPoint: "hangAfterStartingIndexBuildUnlocked", mode: "alwaysOn"}),
);
assert.commandWorked(secondaryDB.adminCommand({configureFailPoint: "failIndexBuildOnCommit", mode: "alwaysOn"}));

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

try {
    // Wait for the index build to start on the primary.
    const opId = IndexBuildTest.waitForIndexBuildToStart(testDB, coll.getName(), "a_1");
    IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId);

    // The index build on the secondary will fail but will wait for the abortIndexBuild oplog entry
    // from the primary.
    const secondaryOpId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB, coll.getName(), "a_1");
    IndexBuildTest.assertIndexBuildCurrentOpContents(secondaryDB, secondaryOpId);
} finally {
    secondaryDB.adminCommand({configureFailPoint: "hangAfterStartingIndexBuildUnlocked", mode: "off"});
    IndexBuildTest.resumeIndexBuilds(primary);
}

// Wait for the index build to stop.
IndexBuildTest.waitForIndexBuildToStop(testDB);

const exitCode = createIdx();
assert.eq(0, exitCode, "expected shell to exit successfully");

// Wait until the secondary process exits. We don't use ReplSetTest.stop() because if the secondary
// hasn't processed the commitIndexBuild oplog entry yet, the node will get signaled to shutdown
// cleanly and return an exit code of 0.
let res;
assert.soon(function () {
    res = checkProgram(secondary.pid);
    return !res.alive;
});

// Secondary should crash on receiving the unexpected commitIndexBuild oplog entry.
assert.eq(MongoRunner.EXIT_ABORT, res.exitCode);
assert(
    rawMongoProgramOutput(".*").match("Fatal assertion.*4698902"),
    "Index build should have aborted secondary due to unexpected commitIndexBuild oplog entry.",
);

// Check indexes on primary.
IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"]);

const cmdNs = testDB.getCollection("$cmd").getFullName();
const ops = rst.dumpOplog(primary, {op: "c", ns: cmdNs, "o.commitIndexBuild": coll.getName()});
assert.eq(1, ops.length, "primary did not write commitIndexBuild oplog entry: " + tojson(ops));

TestData.skipCheckDBHashes = true;
rst.stopSet();
