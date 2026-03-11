/**
 * Initial syncing a node with unfinished two phase index builds when using the default
 * "votingMembers" commit quorum can hold up an index build unless the commit quorum is reduced,
 * as requiring the vote of an initial syncing node can significantly delay completion of the index
 * build on the primary (and other secondaries).
 *
 * @tags: [
 *   requires_commit_quorum,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

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
/** @type {Mongo} */
let secondary = rst.getSecondary();
const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

assert.commandWorked(coll.insert({a: 1, b: 1, c: 1, d: 1, e: 1, f: 1, g: 1}));
assert.commandWorked(coll.createIndex({a: 1}, {}, "votingMembers"));
rst.awaitReplication();

// Start multiple index builds using a commit quorum of "votingMembers", but pause the index build
// on the secondary, preventing it from voting to commit the index build.
jsTest.log("Pausing commitIndexBuild on the primary and starting index builds on the secondary");
const commitFp = configureFailPoint(primary, "hangIndexBuildBeforeCommit");
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

jsTest.log("Restarting secondary and beginning initial sync");
const initialSyncFpName = "initialSyncHangDuringCollectionClone";

function fpDataToStartupParams(failpoints) {
    const startupParams = {};
    for (const [key, value] of Object.entries(failpoints)) {
        startupParams["failpoint." + key] = tojson(value);
    }
    return startupParams;
}

let fpData = {};
fpData[initialSyncFpName] = {mode: "alwaysOn", data: {namespace: `${dbName}.${collName}`, numDocsToClone: 0}};

// Restart the secondary with a clean data directory to start the initial sync process.
secondary = rst.restart(1, {
    startClean: true,
    setParameter: fpDataToStartupParams(fpData),
});

// Once the secondary is undergoing initial sync, the primary should be able to move forward by
// determining that the commit quorum is satisfied, despite the fact that the secondary will be
// hung in the collection cloning phase prior to starting any unfinished index builds.
assert.commandWorked(
    secondary.adminCommand({
        waitForFailPoint: initialSyncFpName,
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

// Cannot use IndexBuildTest helper functions on the secondary during initial sync.
function checkIndexBuildStarted(conn, indexes, expectStarted = true) {
    for (let i = 0; i < indexes.length; i++) {
        const checkFn = expectStarted
            ? (conn, id, attrs) => {
                  return checkLog.containsJson(conn, id, attrs);
              }
            : (conn, id, attrs) => {
                  return !checkLog.checkContainsOnceJson(conn, id, attrs);
              };

        checkFn(conn, 20384, {
            "properties": function (obj) {
                return obj.name === indexes[i];
            },
        });
    }
}

checkIndexBuildStarted(secondary, ["b_1", "c_1", "d_1", "e_1", "f_1", "g_1"], false);

jsTest.log("Waiting for index builds to be ready to commit");
const indexBuildGroups = [["b_1"], ["c_1", "d_1"], ["e_1", "f_1", "g_1"]];

// Ensure that all in-progress index builds are awaiting votes.
IndexBuildTest.checkLogByBuildUUIDForIndexes(coll, 3856203, ["a_1"], indexBuildGroups.flat());

startParallelShell(
    funWithArgs(
        (collName, indexBuildGroups) => {
            indexBuildGroups.forEach((indexNames) => {
                assert.commandWorked(
                    db.runCommand({
                        setIndexCommitQuorum: collName,
                        indexNames: indexNames,
                        commitQuorum: "majority",
                    }),
                );
            });
        },
        coll.getName(),
        indexBuildGroups,
    ),
    primary.port,
);

// N.B. This waits for 2x the index builds because the failpoint is "hit" twice for each:
// once on shouldFail() and once on pauseWhileSet(..) when enabled.
commitFp.wait({timesEntered: indexBuildGroups.length * 2});
commitFp.off();

jsTest.log("Ready index builds have satisfied commit quorum and are ready to commit");

awaitFirstIndexBuild();
awaitSecondIndexBuild();
awaitThirdIndexBuild();

IndexBuildTest.assertIndexesIdHelper(coll, 7, ["a_1", "b_1", "c_1", "d_1", "e_1", "f_1", "g_1"]);

checkIndexBuildStarted(secondary, ["b_1", "c_1", "d_1", "e_1", "f_1", "g_1"], false);
assert.commandWorked(secondary.adminCommand({configureFailPoint: initialSyncFpName, mode: "off"}));
checkIndexBuildStarted(secondary, ["b_1", "c_1", "d_1", "e_1", "f_1", "g_1"]);

rst.awaitReplication();
rst.awaitSecondaryNodes();

const secondaryColl = secondary.getDB(dbName).getCollection(collName);
IndexBuildTest.assertIndexesIdHelper(coll, 7, ["a_1", "b_1", "c_1", "d_1", "e_1", "f_1", "g_1"]);
IndexBuildTest.assertIndexesIdHelper(secondaryColl, 7, ["a_1", "b_1", "c_1", "d_1", "e_1", "f_1", "g_1"]);
rst.stopSet();
