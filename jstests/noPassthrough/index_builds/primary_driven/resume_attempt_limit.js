/**
 * Tests that a primary-driven index build that keeps crashing new primaries without persisting
 * progress is aborted past maxIndexBuildAttemptsAfterUncleanShutdown, and that resumes that persist
 * progress reset the count.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, describe, it} from "jstests/libs/mochalite.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {
    PdibFailoverMode,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const dbName = jsTestName();
const kLimit = 2;
const kScanFp = "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion";
const kInitializedFp = "hangAfterInitializingIndexBuild";

// Unclean failovers take the primary down before the election, so a third node keeps a majority.
const rst = PrimaryDrivenResumableIndexBuildTest.setUp({testName: jsTestName(), nodes: 3});
if (!PrimaryDrivenResumableIndexBuildTest._preflight(rst, dbName, jsTestName())) {
    PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
    quit();
}
// The harness reapplies these when a failover restarts a node.
PrimaryDrivenResumableIndexBuildTest._setIndexBuildSettings(rst, {
    maxIndexBuildAttemptsAfterUncleanShutdown: kLimit,
    maxIndexBuildMemoryUsageMegabytes: 1,
    logComponentVerbosity: {index: {verbosity: 1}},
});

// Starts building {a: 1}, pauses the scan at '_id: stopId', and returns {buildUUID, awaitBuild}.
function startHungBuild(coll, stopId) {
    const primary = rst.getPrimary();
    const fp = configureFailPoint(primary, kScanFp, {fieldsToMatch: {_id: stopId}});
    const awaitBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});
    fp.wait();
    const indexMap = IndexBuildTest.assertIndexesSoon(coll, 2, ["_id_"], ["a_1"], {
        includeBuildUUIDs: true,
    });
    // The killed primary's resume state must already be on the node that resumes it.
    rst.awaitReplication();
    return {buildUUID: extractUUIDFromObject(indexMap["a_1"].buildUUID), awaitBuild};
}

// Kills the primary and returns the new primary and its configured fail point.
function crashAndResume(primary, failPointName, data = {}) {
    const fps = new Map(
        rst
            .getSecondaries()
            .map((node) => [node.host, configureFailPoint(node, failPointName, data)]),
    );
    const newPrimary = PrimaryDrivenResumableIndexBuildTest._failover(
        rst,
        primary,
        rst.getSecondaries()[0],
        PdibFailoverMode.UNCLEAN_RESTART,
    );
    return {newPrimary, fp: fps.get(newPrimary.host)};
}

function offEverywhere(failPointName) {
    for (const node of rst.nodes) {
        assert.commandWorked(node.adminCommand({configureFailPoint: failPointName, mode: "off"}));
    }
}

function exceededAttemptLimit(node) {
    return assert.commandWorked(node.adminCommand({serverStatus: 1})).indexBuilds
        .exceededAttemptLimit;
}

function assertBuildAborted(primary, collName, buildUUID) {
    const oplog = primary.getDB("local").getCollection("oplog.rs");
    assert.soon(() =>
        oplog.findOne({
            op: "c",
            "o.abortIndexBuild": {$exists: true},
            "o.indexBuildUUID": UUID(buildUUID),
            "o.cause.code": ErrorCodes.IndexBuildAborted,
        }),
    );
    rst.awaitReplication();
    for (const node of rst.nodes) {
        IndexBuildTest.assertIndexes(node.getDB(dbName).getCollection(collName), 1, ["_id_"]);
    }
    assert.eq(1, exceededAttemptLimit(primary));
}

describe("primary-driven index build resume limit", function () {
    after(function () {
        PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
    });

    it("aborts a build that never gets past the same document", function () {
        const coll = rst.getPrimary().getDB(dbName).noProgress;
        // Small documents never fill the sorter, so nothing spills and no resume makes progress.
        assert.commandWorked(coll.insert(Array.from({length: 100}, (_, i) => ({_id: i, a: i}))));
        rst.awaitReplication();

        const {buildUUID, awaitBuild} = startHungBuild(coll, 50);
        let primary = rst.getPrimary();
        // The first resume starts the count at 0, and each later one without progress adds one, so
        // kLimit + 1 resumes run before the next one is over the limit.
        for (let resume = 0; resume <= kLimit; resume++) {
            // Reaching the same document again means this new primary resumed the build.
            const {newPrimary, fp} = crashAndResume(primary, kScanFp, {fieldsToMatch: {_id: 50}});
            fp.wait();
            rst.awaitReplication();
            primary = newPrimary;
        }

        const {newPrimary} = crashAndResume(primary, kScanFp, {fieldsToMatch: {_id: 50}});
        assertBuildAborted(newPrimary, "noProgress", buildUUID);

        offEverywhere(kScanFp);
        awaitBuild({checkExitSuccess: false});
        assert(newPrimary.getDB(dbName).noProgress.drop());
    });

    it("keeps resuming a build that spills further each time", function () {
        const coll = rst.getPrimary().getDB(dbName).progress;
        // About 16 documents of 64 KB fill the 1 MB sorter. The first stop comes before any spill,
        // and the 40 documents between later stops always spill again.
        const largePadding = "x".repeat(64 * 1024);
        assert.commandWorked(
            coll.insert(Array.from({length: 200}, (_, i) => ({_id: i, a: `${i}-${largePadding}`}))),
        );
        rst.awaitReplication();

        // More resumes than the limit allows, so the build only finishes if the count resets.
        const stops = [10, 50, 90, 130, 170];
        const {buildUUID, awaitBuild} = startHungBuild(coll, stops[0]);
        let primary = rst.getPrimary();
        for (const nextStop of stops.slice(1)) {
            const {newPrimary, fp} = crashAndResume(primary, kScanFp, {
                fieldsToMatch: {_id: nextStop},
            });
            fp.wait();
            rst.awaitReplication();
            primary = newPrimary;
        }
        for (const node of rst.nodes) {
            assert.eq(0, exceededAttemptLimit(node), {host: node.host});
        }

        offEverywhere(kScanFp);
        const oplog = primary.getDB("local").getCollection("oplog.rs");
        assert.soon(() =>
            oplog.findOne({
                op: "c",
                "o.commitIndexBuild": {$exists: true},
                "o.indexBuildUUID": UUID(buildUUID),
            }),
        );
        awaitBuild({checkExitSuccess: false});
        const res = assert.commandWorked(primary.getDB(dbName).progress.validate());
        assert(res.valid, "validate failed", {res});
        assert(primary.getDB(dbName).progress.drop());
    });

    it("planned handoffs neither increment nor reset earlier unplanned resumes", function () {
        if (
            !TestData.doesNotSupportGracefulUnplannedStepdown ||
            TestData.doesNotSupportGracefulPlannedStepdown
        ) {
            jsTest.log.info("Skipping planned-handoff counting test without DSC handoff support");
            return;
        }

        const coll = rst.getPrimary().getDB(dbName).handoff;
        assert.commandWorked(coll.insert(Array.from({length: 100}, (_, i) => ({_id: i, a: i}))));
        rst.awaitReplication();
        const {buildUUID, awaitBuild} = startHungBuild(coll, 50);
        let primary = rst.getPrimary();
        // Accumulate unplanned resumes without progress.
        for (let resume = 0; resume <= kLimit; resume++) {
            const {newPrimary, fp} = crashAndResume(primary, kScanFp, {fieldsToMatch: {_id: 50}});
            fp.wait();
            rst.awaitReplication();
            primary = newPrimary;
        }

        // Perform several planned handoffs, still without progress.
        for (let handoff = 0; handoff <= kLimit; handoff++) {
            const nextPrimary = rst.getSecondaries()[0];
            const fp = configureFailPoint(nextPrimary, kScanFp, {fieldsToMatch: {_id: 50}});
            primary = PrimaryDrivenResumableIndexBuildTest._failOverWithStepDown(
                rst,
                primary,
                nextPrimary,
            );
            fp.wait();
            rst.awaitReplication();
            assert.eq(0, exceededAttemptLimit(primary));
        }

        // Perform one more unplanned handoff, which aborts the build.
        const {newPrimary} = crashAndResume(primary, kScanFp, {fieldsToMatch: {_id: 50}});
        assertBuildAborted(newPrimary, "handoff", buildUUID);
        offEverywhere(kScanFp);
        awaitBuild({checkExitSuccess: false});
        assert(newPrimary.getDB(dbName).handoff.drop());
    });

    it("aborts a multi-index build interrupted before persisting resume state", function () {
        let primary = rst.getPrimary();
        const coll = primary.getDB(dbName).initialized;
        assert.commandWorked(coll.insert({_id: 0, a: 1, b: 2}));
        rst.awaitReplication();

        // This pause is before the initialized to scan phase change persists the first resume state.
        const fp = configureFailPoint(primary, kInitializedFp);
        const awaitBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), [
            {a: 1},
            {b: 1},
        ]);
        fp.wait();
        const indexMap = IndexBuildTest.assertIndexesSoon(coll, 3, ["_id_"], ["a_1", "b_1"], {
            includeBuildUUIDs: true,
        });
        const buildUUID = extractUUIDFromObject(indexMap["a_1"].buildUUID);
        rst.awaitReplication();

        for (let resume = 0; resume <= kLimit; resume++) {
            const result = crashAndResume(primary, kInitializedFp);
            result.fp.wait();
            rst.awaitReplication();
            primary = result.newPrimary;
        }

        const {newPrimary} = crashAndResume(primary, kInitializedFp);
        assertBuildAborted(newPrimary, "initialized", buildUUID);

        offEverywhere(kInitializedFp);
        awaitBuild({checkExitSuccess: false});
        assert(newPrimary.getDB(dbName).initialized.drop());
    });
});
