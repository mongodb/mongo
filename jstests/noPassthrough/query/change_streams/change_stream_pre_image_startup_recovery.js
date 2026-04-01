/**
 * Tests that pre-image truncation performed before a shutdown is still reflected after
 * recovery. Works for both truncation paths:
 *
 * Replicated truncates:
 *   1. Pre-images are inserted and checkpointed to disk.
 *   2. Checkpointing is paused.
 *   3. The remover job truncates expired pre-images (replicated deletes go to oplog
 *      but are NOT flushed to disk since checkpointing is paused).
 *   4. The node is crashed (SIGKILL).
 *   5. On restart, oplog replay re-applies the replicated deletes.
 *   6. The truncated pre-images must not reappear.
 *
 * Unreplicated truncates (legacy):
 *   Steps 1-4 are the same, but the truncation is unreplicated (untimestamped WT truncate).
 *   On restart, cleanupPreImagesCollectionAfterUncleanShutdown() re-truncates expired
 *   pre-images. The changeStreamPreImageRemoverCurrentTime failpoint controls the time
 *   used by startup recovery so the 10-second extension buffer doesn't over-truncate.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {describe, it, before, beforeEach, afterEach, after} from "jstests/libs/mochalite.js";
import {getPreImages, getPreImagesCollection} from "jstests/libs/query/change_stream_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// kChangeStreamPostUncleanShutdownExpiryExtensionSeconds from startup_recovery.h.
const kExtensionSecs = 10;
const kExpireAfterSeconds = 3;

describe("change stream pre-image truncation persists across crash + recovery", function () {
    let rst;
    let primary;
    let dbName;
    let coll;
    let caseCounter = 0;

    before(function () {
        rst = new ReplSetTest({
            // Set priority 1 on both nodes to prevent priority 0 from being auto-assigned to the
            // standby in disagg testing.
            nodes: [{rsConfig: {priority: 1}}, {rsConfig: {priority: 1}}],
            nodeOptions: {
                setParameter: {
                    expiredChangeStreamPreImageRemovalJobSleepSecs: 1,
                    preImagesCollectionTruncateMarkersMinBytes: 1,
                    // Lower from 60s default so checkpointAndPauseThread returns
                    // quickly (it waits up to syncdelay for the thread to hit the failpoint).
                    syncdelay: 5,
                },
            },
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        dbName = jsTestName();
    });

    beforeEach(function () {
        primary = rst.getPrimary();
        const testDB = primary.getDB(dbName);
        const collName = "test_" + caseCounter++;
        coll = testDB[collName];

        assert.commandWorked(
            primary.getDB("admin").runCommand({
                setClusterParameter: {
                    changeStreamOptions: {
                        preAndPostImages: {expireAfterSeconds: kExpireAfterSeconds},
                    },
                },
            }),
        );

        assert.commandWorked(
            testDB.createCollection(coll.getName(), {
                changeStreamPreAndPostImages: {enabled: true},
            }),
        );

        assert.commandWorked(coll.insert({_id: 0, v: 0}));
    });

    afterEach(function () {
        // Delete all pre-images to prevent cross-test contamination.
        assert.commandWorked(
            primary.getDB("config").runCommand({
                delete: "system.preimages",
                deletes: [{q: {}, limit: 0}],
            }),
        );
        rst.awaitReplication();
        primary.getDB(dbName).dropDatabase();

        // Reset failpoints on all live nodes.
        for (const node of rst.nodes) {
            try {
                assert.commandWorked(
                    node.adminCommand({configureFailPoint: "changeStreamPreImageRemoverCurrentTime", mode: "off"}),
                );
                assert.commandWorked(node.adminCommand({configureFailPoint: "pauseCheckpointThread", mode: "off"}));
            } catch (e) {
                // Node may be stopped after a crash test.
            }
        }
    });

    after(function () {
        rst.stopSet();
    });

    /** Generates pre-images by updating {_id: 0} `count` times. */
    function generatePreImages(count) {
        for (let i = 0; i < count; ++i) {
            assert.commandWorked(coll.update({_id: 0}, {$inc: {v: 1}}));
        }
    }

    /** Returns the operationTime of the latest pre-image, or the current cluster time if none. */
    function getLatestPreImageTime() {
        const preImages = getPreImagesCollection(primary).find().sort({"_id.ts": -1}).limit(1).toArray();
        if (preImages.length > 0) {
            return preImages[0].operationTime.getTime();
        }
        return assert.commandWorked(primary.getDB(dbName).runCommand({ping: 1})).operationTime.getTime();
    }

    /**
     * Returns a time that makes the live remover expire all pre-images at or before
     * referenceOpTime. Used BEFORE crash to trigger truncation.
     */
    function computeExpireTime(referenceOpTime) {
        return new Date(referenceOpTime + (kExpireAfterSeconds + 1) * 1000);
    }

    /**
     * Returns a time for the restart failpoint that makes startup recovery truncate
     * exactly the expired pre-images and keeps the live remover inert.
     *
     * threshold = result - expireAfterSeconds + extensionSecs = referenceOpTime.
     * For the replicated path, startup recovery returns early, so this is harmless.
     */
    function computeRestartRemoverTime(referenceOpTime) {
        return new Date(referenceOpTime - (kExtensionSecs - kExpireAfterSeconds) * 1000);
    }

    /**
     * Checkpoints `node` to disk, then pauses its checkpoint thread. Operations after
     * this call are NOT persisted to disk until the node is killed (which clears the
     * failpoint). The caller MUST crash the node before it is stopped cleanly.
     */
    function checkpointAndPauseThread(node) {
        assert.commandWorked(node.adminCommand({fsync: 1}));
        configureFailPoint(node, "pauseCheckpointThread").wait();
    }

    /** Injects a fake "current time" into the pre-image remover on `node`. */
    function setRemoverCurrentTime(node, date) {
        assert.commandWorked(
            node.adminCommand({
                configureFailPoint: "changeStreamPreImageRemoverCurrentTime",
                mode: "alwaysOn",
                data: {currentTimeForTimeBasedExpiration: date.toISOString()},
            }),
        );
    }

    /**
     * Freezes the remover by setting its time to epoch. The expiry threshold becomes
     * negative, so no real pre-image can ever qualify for removal.
     */
    function freezePreImageTruncationJob(node) {
        setRemoverCurrentTime(node, new Date(0));
    }

    /** Polls until exactly `remaining` pre-images are left on `node`. */
    function waitForPreImageTruncation(node, remaining) {
        assert.soon(
            () => getPreImages(node).length === remaining,
            () => "timed out waiting for remover to truncate on " + node.host + ": " + tojson(getPreImages(node)),
        );
    }

    /**
     * Shuts down the node, restarts with the remover time failpoint set, and returns
     * the restarted connection. If the primary was shut down, steps up the surviving
     * node (ReplSetTest uses 24h electionTimeoutMillis, so no auto-election occurs).
     */
    function crashAndRestart(nodeToTest, {cleanShutdown, removerCurrentTime}) {
        const nodeId = rst.getNodeId(nodeToTest);
        const nodeWasPrimary = nodeToTest.host === rst.getPrimary().host;

        if (cleanShutdown) {
            jsTest.log.info("Performing clean shutdown of node", {host: nodeToTest.host});
            rst.stop(nodeToTest);
        } else {
            jsTest.log.info("Forcing unclean shutdown (SIGKILL) of node", {host: nodeToTest.host});
            rst.stop(nodeToTest, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
        }

        // Set the remover time on restart: either the computed time (unclean) or epoch (clean).
        // Both prevent the live remover from truncating during our assertions.
        // Merge the failpoint into the node's saved setParameter to preserve
        // syncdelay, expiredChangeStreamPreImageRemovalJobSleepSecs, etc.
        const removerTime = !cleanShutdown && removerCurrentTime ? removerCurrentTime : new Date(0);
        const saved = rst.nodes[nodeId].fullOptions.setParameter || {};
        const restartOpts = {
            setParameter: Object.assign({}, saved, {
                "failpoint.changeStreamPreImageRemoverCurrentTime": tojsononeline({
                    mode: "alwaysOn",
                    data: {currentTimeForTimeBasedExpiration: removerTime.toISOString()},
                }),
            }),
        };

        const restarted = rst.start(rst.nodes[nodeId], restartOpts, /* restart */ true);

        assert.soonNoExcept(() => {
            const st = assert.commandWorked(restarted.adminCommand("replSetGetStatus")).myState;
            return st === ReplSetTest.State.SECONDARY;
        });
        restarted.setSecondaryOk();

        if (nodeWasPrimary) {
            const otherNodeId = 1 - nodeId;
            const otherNode = rst.nodes[otherNodeId];

            // Freeze the surviving node's remover BEFORE step-up. onStepUpComplete() starts
            // the remover, which would expire all pre-images with real wall-clock time and
            // replicate those deletions to the restarted secondary.
            freezePreImageTruncationJob(otherNode);

            assert.soonNoExcept(() => {
                assert.commandWorked(otherNode.adminCommand({replSetFreeze: 0}));
                assert.commandWorked(otherNode.adminCommand({replSetStepUp: 1}));
                return true;
            }, "Failed to step up node " + otherNode.host);

            rst.waitForPrimary();
        }

        return restarted;
    }

    it("primary clean shutdown with some expired pre-images", function () {
        generatePreImages(4);
        rst.awaitReplication();

        const referenceOpTime = getLatestPreImageTime();
        const expireAt = computeExpireTime(referenceOpTime);

        setRemoverCurrentTime(primary, expireAt);
        waitForPreImageTruncation(primary, 0 /* remaining */);
        freezePreImageTruncationJob(primary);

        // Pre-image expiry compares operationTime at second granularity.
        // Ensure the unexpired batch lands in a different wall-clock second.
        sleep(1001);

        generatePreImages(2);
        rst.awaitReplication();

        const nodeToTest = rst.getPrimary();
        const preImagesBefore = getPreImages(nodeToTest);
        jsTest.log.info("Pre-images before clean shutdown", {preImagesBefore});

        const restarted = crashAndRestart(nodeToTest, {
            cleanShutdown: true,
            removerCurrentTime: null,
        });

        rst.waitForPrimary();
        primary = rst.getPrimary();

        const preImagesAfter = getPreImages(restarted);
        jsTest.log.info("Pre-images after clean restart", {preImagesAfter});

        assert.eq(preImagesBefore, preImagesAfter, "pre-images changed after recovery");
    });

    it("primary unclean shutdown with some expired pre-images", function () {
        generatePreImages(4);
        rst.awaitReplication();

        const referenceOpTime = getLatestPreImageTime();
        const expireAt = computeExpireTime(referenceOpTime);

        // Freeze the truncation job to avoid pre-images being removed.
        freezePreImageTruncationJob(primary);

        // Pre-image expiry compares operationTime at second granularity (the failpoint
        // accepts a Date, which has no sub-second Timestamp increment). Ensure the
        // unexpired batch lands in a different wall-clock second.
        sleep(1001);

        generatePreImages(2);
        rst.awaitReplication();

        const nodeToTest = rst.getPrimary();

        // Checkpoint all pre-images, then pause the thread. Truncations after
        // this point are NOT persisted, so crash recovery must redo them.
        checkpointAndPauseThread(nodeToTest);

        setRemoverCurrentTime(primary, expireAt);
        waitForPreImageTruncation(primary, 2 /* remaining */);
        freezePreImageTruncationJob(primary);

        const preImagesBefore = getPreImages(nodeToTest);
        jsTest.log.info("Pre-images before crash", {preImagesBefore});

        const restarted = crashAndRestart(nodeToTest, {
            cleanShutdown: false,
            removerCurrentTime: computeRestartRemoverTime(referenceOpTime),
        });

        rst.waitForPrimary();
        primary = rst.getPrimary();

        const preImagesAfter = getPreImages(restarted);
        jsTest.log.info("Pre-images after restart", {preImagesAfter});

        assert.eq(preImagesBefore, preImagesAfter, "pre-images changed after recovery");
    });

    it("secondary unclean shutdown with some expired pre-images", function () {
        generatePreImages(4);
        rst.awaitReplication();

        const nodeToTest = rst.getSecondary();

        // Freeze the truncation job to avoid pre-images being removed.
        const referenceOpTime = getLatestPreImageTime();
        const expireAt = computeExpireTime(referenceOpTime);
        freezePreImageTruncationJob(primary);
        freezePreImageTruncationJob(nodeToTest);

        // Pre-image expiry compares operationTime at second granularity.
        // Ensure the unexpired batch lands in a different wall-clock second.
        sleep(1001);

        // Checkpoint, then pause the secondary's checkpoint thread.
        // Truncations after this point are NOT persisted on the secondary.
        checkpointAndPauseThread(nodeToTest);

        generatePreImages(2);
        rst.awaitReplication();
        assert.commandWorked(nodeToTest.adminCommand({fsync: 1}));

        // Truncate on primary first.
        setRemoverCurrentTime(primary, expireAt);
        waitForPreImageTruncation(primary, 2 /* remaining */);
        freezePreImageTruncationJob(primary);

        rst.awaitReplication();
        nodeToTest.setSecondaryOk();

        // On the unreplicated path the secondary runs its own remover independently,
        // so we must inject time and wait for truncation on it separately.
        setRemoverCurrentTime(nodeToTest, expireAt);
        waitForPreImageTruncation(nodeToTest, 2 /* remaining */);
        freezePreImageTruncationJob(nodeToTest);

        const preImagesBefore = getPreImages(nodeToTest);
        jsTest.log.info("Pre-images before crash", {preImagesBefore});

        const restarted = crashAndRestart(nodeToTest, {
            cleanShutdown: false,
            removerCurrentTime: computeRestartRemoverTime(referenceOpTime),
        });

        const preImagesAfter = getPreImages(restarted);
        jsTest.log.info("Pre-images after restart", {preImagesAfter});

        assert.eq(preImagesBefore, preImagesAfter, "pre-images changed after recovery");
    });

    it("primary unclean shutdown with all pre-images expired", function () {
        generatePreImages(4);
        rst.awaitReplication();

        const referenceOpTime = getLatestPreImageTime();
        const expireAt = computeExpireTime(referenceOpTime);

        // Freeze the truncation job to avoid pre-images being removed.
        freezePreImageTruncationJob(primary);

        checkpointAndPauseThread(primary);

        setRemoverCurrentTime(primary, expireAt);
        waitForPreImageTruncation(primary, 0 /* remaining */);
        freezePreImageTruncationJob(primary);

        const preImagesBefore = getPreImages(primary);
        jsTest.log.info("Pre-images before crash (all expired)", {preImagesBefore});

        const restarted = crashAndRestart(primary, {
            cleanShutdown: false,
            removerCurrentTime: computeRestartRemoverTime(referenceOpTime),
        });

        rst.waitForPrimary();
        primary = rst.getPrimary();

        const preImagesAfter = getPreImages(restarted);
        jsTest.log.info("Pre-images after restart (all expired)", {preImagesAfter});

        assert.eq(preImagesBefore, preImagesAfter, "pre-images changed after recovery");
    });

    it("primary unclean shutdown with no expired pre-images", function () {
        const referenceOpTime = getLatestPreImageTime();
        const nodeToTest = rst.getPrimary();

        checkpointAndPauseThread(nodeToTest);

        generatePreImages(4);
        rst.awaitReplication();
        assert.commandWorked(nodeToTest.adminCommand({fsync: 1}));

        const preImagesBefore = getPreImages(nodeToTest);
        jsTest.log.info("Pre-images before crash (none expired)", {preImagesBefore});
        assert.gte(preImagesBefore.length, 4, "expected at least 4 pre-images");

        const restarted = crashAndRestart(nodeToTest, {
            cleanShutdown: false,
            removerCurrentTime: computeRestartRemoverTime(referenceOpTime),
        });

        rst.waitForPrimary();
        primary = rst.getPrimary();

        const preImagesAfter = getPreImages(restarted);
        jsTest.log.info("Pre-images after restart (none expired)", {preImagesAfter});

        assert.eq(preImagesBefore, preImagesAfter, "pre-images changed after recovery");
    });
});
