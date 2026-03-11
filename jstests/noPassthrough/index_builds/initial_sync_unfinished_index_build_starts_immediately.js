/**
 * Initial syncing a node with two phase index builds should immediately build all ready indexes
 * from the sync source and only setup the index builder threads for any unfinished index builds
 * grouped by their buildUUID.
 *
 * Since the primary only writes the "commitIndexBuild" oplog entry once the commit quorum is
 * satisfied by receiving the expected number of votes (default is all "votingMembers"), an initial
 * syncing node should not wait for applying that oplog entry to start the index build.
 *
 * Although the number of votingMembers can be lowered to avoid the initial syncing node,
 * starting the index build as soon as possible helps ensure that initial syncing ends sooner,
 * since once the "commitIndexBuild" oplog entry has been written by the primary and replicated,
 * further oplog entry application will be blocked on the initial syncing node until the index
 * build is complete.
 *
 * @tags: [
 *   requires_commit_quorum,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const dbName = "test";
const collName = jsTestName();

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
    ],
});

rst.startSet();
rst.initiate();

/** @type {Mongo} */
const primary = rst.getPrimary();
const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

assert.commandWorked(coll.insert({a: 1, b: 1, c: 1, d: 1, e: 1, f: 1, g: 1}));
assert.commandWorked(coll.createIndex({a: 1}, {}, "votingMembers"));
rst.awaitReplication();

// Start multiple index builds using a commit quorum of "votingMembers", but pause the index build
// on the secondary, preventing it from voting to commit the index build.
jsTest.log("Pausing index builds on the secondary");
let secondary = rst.getSecondary();
IndexBuildTest.pauseIndexBuilds(secondary);

const awaitFirstIndexBuild = IndexBuildTest.startIndexBuild(
    primary,
    coll.getFullName(),
    {b: 1},
    {},
    [],
    "votingMembers",
);
const awaitSecondIndexBuild = IndexBuildTest.startIndexBuild(
    primary,
    coll.getFullName(),
    [{c: 1}, {d: 1}],
    {},
    [],
    "votingMembers",
);
const awaitThirdIndexBuild = IndexBuildTest.startIndexBuild(
    primary,
    coll.getFullName(),
    [{e: 1}, {f: 1}, {g: 1}],
    {},
    "votingMembers",
);

// Wait for all the indexes to start building on the primary.
IndexBuildTest.waitForIndexBuildToStart(db, collName, "b_1");
IndexBuildTest.waitForIndexBuildToStart(db, collName, "c_1");
IndexBuildTest.waitForIndexBuildToStart(db, collName, "d_1");
IndexBuildTest.waitForIndexBuildToStart(db, collName, "e_1");
IndexBuildTest.waitForIndexBuildToStart(db, collName, "f_1");
IndexBuildTest.waitForIndexBuildToStart(db, collName, "g_1");

// Restart the secondary with a clean data directory to start the initial sync process.
secondary = rst.restart(1, {
    startClean: true,
    setParameter: "failpoint.initialSyncHangAfterDataCloning=" + tojson({mode: "alwaysOn"}),
});

// The secondary node will start any in-progress two-phase index builds from the primary before
// starting the oplog replay phase. This ensures that the secondary will send its vote to the
// primary when it is ready to commit the index build. The index build on the secondary will get
// committed once the primary sends the "commitIndexBuild" oplog entry after the commit quorum is
// satisfied with the secondaries vote.
checkLog.containsJson(secondary, 21184);

// Cannot use IndexBuildTest helper functions on the secondary during initial sync.
function checkForIndexes(indexes) {
    for (let i = 0; i < indexes.length; i++) {
        checkLog.containsJson(secondary, 20384, {
            "properties": function (obj) {
                return obj.name === indexes[i];
            },
        });
    }
}
checkForIndexes(["b_1", "c_1", "d_1", "e_1", "f_1", "g_1"]);

assert.commandWorked(secondary.adminCommand({configureFailPoint: "initialSyncHangAfterDataCloning", mode: "off"}));

rst.awaitReplication();
rst.awaitSecondaryNodes();

awaitFirstIndexBuild();
awaitSecondIndexBuild();
awaitThirdIndexBuild();

let indexes = secondary.getDB(dbName).getCollection(collName).getIndexes();
assert.eq(8, indexes.length);

indexes = coll.getIndexes();
assert.eq(8, indexes.length);
rst.stopSet();
