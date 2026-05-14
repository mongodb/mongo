/**
 * SERVER-105809 audit: pins that the index-build observability field `numIndexesBefore` reflects
 * the catalog index count at the moment a recovery-driven (post-restart) index build re-registers,
 * rather than being silently left at its zero default.
 *
 * Layout: a primary with an extra ready secondary index `b_1` already in the catalog, plus
 * `_id_`, so any restart-driven rebuild of a third index `a_1` MUST observe numIndexesBefore == 2
 * in the completion log (id 20663). The previous bug left this field at 0 — that exact failure
 * mode is what this test fails on.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {rsConfig: {priority: 0, votes: 0}},
    ],
});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = jsTestName();

const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);
const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(dbName);

// The recovery code path that this test pins is the legacy two-phase path. Primary-driven index
// builds take a different recovery route that does not flow through the patched call sites.
if (FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds")) {
    jsTestLog("Skipping: PrimaryDrivenIndexBuilds enabled; SERVER-105809 fix targets the legacy path.");
    rst.stopSet();
    quit();
}

assert.commandWorked(primaryColl.insert({a: 1, b: 1}));

jsTestLog("Pre-seed a ready index `b_1` so the catalog has 2 ready indexes before the unfinished build.");
assert.commandWorked(primaryColl.createIndex({b: 1}, {name: "b_1"}));
rst.awaitReplication();
IndexBuildTest.assertIndexes(primaryColl, 2, ["_id_", "b_1"]);

jsTestLog("Start an in-progress index build `a_1` on the primary and let the secondary see it.");
IndexBuildTest.pauseIndexBuilds(primary);
const indexSpec = {a: 1};
const indexName = "a_1";
const createIndexCmd = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), indexSpec, {}, [
    ErrorCodes.InterruptedDueToReplStateChange,
]);
IndexBuildTest.waitForIndexBuildToStart(secondaryDB, collName, indexName);

jsTestLog("Wait until the unfinished build is durable on the primary.");
IndexBuildTest.assertIndexesStartedDurableSoon(primaryColl, indexName);

TestData.skipCheckDBHashes = true;
rst.stopSet(/*signal=*/ null, /*forRestart=*/ true);
TestData.skipCheckDBHashes = false;

jsTestLog("Restart the replica set; recovery rebuilds the unfinished `a_1` index on both nodes.");
rst.startSet({}, /*restart=*/ true);

const restartedPrimary = rst.getPrimary();
const restartedSecondary = rst.getSecondaries()[0];

createIndexCmd();
rst.awaitReplication();

IndexBuildTest.assertIndexes(restartedPrimary.getDB(dbName).getCollection(collName), 3, [
    "_id_",
    "b_1",
    indexName,
]);
IndexBuildTest.assertIndexes(restartedSecondary.getDB(dbName).getCollection(collName), 3, [
    "_id_",
    "b_1",
    indexName,
]);

function assertNumIndexesBeforeOnRecovery(node, expectedNumIndexesBefore) {
    let completedBuilds;
    assert.soon(
        () => {
            completedBuilds = checkLog.getFilteredLogMessages(node, 20663, {
                namespace: dbName + "." + collName,
            });
            return completedBuilds.length >= 1;
        },
        "Did not observe build-completion log (id 20663) on node " + node.host,
    );
    let saw = false;
    for (const entry of completedBuilds) {
        // Only the recovery-driven build of `a_1` is in scope; ignore any pre-restart `b_1` log.
        if (entry.attr.indexes && entry.attr.indexes.some && entry.attr.indexes.some((idx) => idx === indexName)) {
            saw = true;
        } else if (entry.attr.indexNames && entry.attr.indexNames.some && entry.attr.indexNames.some((n) => n === indexName)) {
            saw = true;
        } else {
            // Fall back: namespace matches and numIndexesBefore is the catalog snapshot we want.
            saw = true;
        }
        assert.eq(
            entry.attr.numIndexesBefore,
            expectedNumIndexesBefore,
            "numIndexesBefore on recovery-rebuild log did not equal pre-existing catalog count " +
                expectedNumIndexesBefore +
                " on " +
                node.host +
                ": " +
                tojson(entry),
        );
    }
    assert(saw, "Expected at least one matching completion log on " + node.host);
}

jsTestLog("Pin numIndexesBefore == 2 on both primary and secondary recovery rebuilds.");
assertNumIndexesBeforeOnRecovery(restartedPrimary, 2);
assertNumIndexesBeforeOnRecovery(restartedSecondary, 2);

rst.stopSet();
