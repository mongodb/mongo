/**
 * Tests that a node whose two-phase index build keeps crashing it stops once it exceeds
 * maxIndexBuildAttemptsAfterUncleanShutdown, and asks the primary to abort the build. If the
 * primary refuses, the node waits for its decision, and builds only if the primary committed.
 *
 * @tags: [
 *   # Primary-driven index builds are not restarted on secondaries after a crash.
 *   primary_driven_index_builds_incompatible,
 *   requires_commit_quorum,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {afterEach, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const kLimit = 1;

// This test crashes nodes, which may cause inaccurate fast counts. With syncdelay's frequent
// checkpoints, a crash can come before the counts are flushed.
TestData.skipEnforceFastCountOnValidate = true;

function testColl(node) {
    return node.getDB("test").getCollection(jsTestName());
}

/**
 * Starts a replica set with 'nodes' and some documents to index.
 */
function startReplSet(nodes) {
    // Frequent checkpoints, so builds become durable quickly enough to survive a crash.
    const rst = new ReplSetTest({nodes, nodeOptions: {syncdelay: 1}});
    rst.startSet();
    rst.initiate();
    assert.commandWorked(testColl(rst.getPrimary()).insert([{a: 1}, {a: 2}, {a: 3}]));
    rst.awaitReplication();
    return rst;
}

const kPauseAttempt = "hangBeforeBuildingIndex";

/**
 * Returns startup parameters that pause the node's restarted attempts, simulating a build that
 * crashes the node.
 */
function pausingParams(extra = {}) {
    return {
        ...extra,
        maxIndexBuildAttemptsAfterUncleanShutdown: kLimit,
        ["failpoint." + kPauseAttempt]: tojson({mode: "alwaysOn"}),
    };
}

/**
 * Returns how many times 'node' has reached kPauseAttempt since it started.
 */
function pausedAttempts(node) {
    return assert.commandWorked(
        node.adminCommand({configureFailPoint: kPauseAttempt, mode: "alwaysOn"}),
    ).count;
}

function kill(rst, nodeId) {
    rst.stop(nodeId, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});
}

/**
 * Starts an index build on 'primary' and pauses the scan on 'node' until it is durable there, so
 * catalog repair restarts it after a crash. Returns the build UUID and a function that waits for
 * createIndexes to finish.
 */
function startBuildAndPauseScan(primary, node, {commitQuorum, expectedFailures = []} = {}) {
    // The build UUID isn't known until the build starts, so pause the scan until the crash
    // failpoint can be scoped to it.
    const scanHang = configureFailPoint(
        node,
        "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
        {
            fieldsToMatch: {a: 1},
        },
    );
    const awaitCreateIndex = IndexBuildTest.startIndexBuild(
        primary,
        testColl(primary).getFullName(),
        {a: 1},
        {},
        expectedFailures,
        commitQuorum,
    );

    scanHang.wait();
    const coll = testColl(node);
    const buildUUID = extractUUIDFromObject(
        IndexBuildTest.assertIndexesSoon(coll, 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true})[
            "a_1"
        ].buildUUID,
    );
    IndexBuildTest.assertIndexesStartedDurableSoon(coll, "a_1");
    return {buildUUID, awaitCreateIndex};
}

/**
 * Kills 'node' during the scan paused by startBuildAndPauseScan and leaves it stopped. Returns its
 * node id.
 */
function crashDuringPausedScan(rst, node, buildUUID) {
    const nodeId = rst.getNodeId(node);
    jsTest.log.info("Killing the node during its first scan", {buildUUID});
    kill(rst, nodeId);
    return nodeId;
}

/**
 * Kills 'node' during the paused scan and then during each restarted attempt until it has attempted
 * the build kLimit + 1 times, and restarts it at the limit. Returns a connection to the node at the
 * limit.
 */
function crashUntilLimit(rst, node, buildUUID, {pauseAtLimit, setParameter = {}}) {
    const params = {...setParameter, maxIndexBuildAttemptsAfterUncleanShutdown: kLimit};
    const pausing = pausingParams(setParameter);

    const nodeId = crashDuringPausedScan(rst, node, buildUUID);
    // Crash the kLimit restarts the limit allows. Attempt 1 crashed above.
    for (let attempt = 2; attempt <= kLimit + 1; attempt++) {
        jsTest.log.info("Restarting the node to kill it during another attempt", {
            buildUUID,
            attempt,
        });
        node = rst.start(nodeId, {setParameter: pausing}, /*restart=*/ true);
        assert.commandWorked(
            node.adminCommand({waitForFailPoint: kPauseAttempt, timesEntered: 1, maxTimeMS: 60000}),
        );
        kill(rst, nodeId);
    }

    node = rst.start(nodeId, {setParameter: pauseAtLimit ? pausing : params}, /*restart=*/ true);
    node.setSecondaryOk();
    return node;
}

/**
 * Returns how many unclean shutdowns interrupted 'node's attempts at the build, or undefined if it
 * has no count.
 */
function attemptsAfterUncleanShutdown(node, buildUUID) {
    return IndexBuildTest.findIndexBuildAttempts(node, buildUUID)?.numAttemptsAfterUncleanShutdown;
}

function assertExceededLimit(node, buildUUID) {
    // Each of the kLimit + 1 attempts ended in a crash.
    assert.eq(kLimit + 1, attemptsAfterUncleanShutdown(node, buildUUID));
    assert.eq(
        1,
        assert.commandWorked(node.adminCommand({serverStatus: 1})).indexBuilds.exceededAttemptLimit,
    );
}

describe("A node that keeps crashing during an index build's scan phase", function () {
    afterEach(() => {
        this.rst.stopSet();
    });

    it("build is aborted on every node when triggered by a secondary", () => {
        this.rst = startReplSet([
            {},
            {
                rsConfig: {priority: 0},
                setParameter: {maxIndexBuildAttemptsAfterUncleanShutdown: kLimit},
            },
        ]);
        const primary = this.rst.getPrimary();

        // Keep the build unfinished while the secondary restarts.
        IndexBuildTest.pauseIndexBuilds(primary);
        // Make the primary wait for the secondary's vote.
        const {buildUUID, awaitCreateIndex} = startBuildAndPauseScan(primary, this.rst.nodes[1], {
            commitQuorum: 2,
            expectedFailures: [ErrorCodes.IndexBuildAborted],
        });
        // Keep pausing attempts at the limit, so the test can check the node doesn't build again.
        const secondary = crashUntilLimit(this.rst, this.rst.nodes[1], buildUUID, {
            pauseAtLimit: true,
        });

        awaitCreateIndex();
        IndexBuildTest.assertIndexesSoon(testColl(primary), 1, ["_id_"]);
        IndexBuildTest.assertIndexesSoon(testColl(secondary), 1, ["_id_"]);
        assertExceededLimit(secondary, buildUUID);
        assert.eq(0, pausedAttempts(secondary));
    });

    it("build is aborted on every node when triggered by a primary", () => {
        this.rst = startReplSet([
            {setParameter: {maxIndexBuildAttemptsAfterUncleanShutdown: kLimit}},
            // Keep the healthy node from taking over while the crashing node is down.
            {rsConfig: {priority: 0}},
        ]);
        let primary = this.rst.getPrimary();

        // Keep the build from committing without the crashing primary.
        const {buildUUID, awaitCreateIndex} = startBuildAndPauseScan(primary, primary, {
            commitQuorum: 2,
        });
        primary = crashUntilLimit(this.rst, primary, buildUUID, {pauseAtLimit: true});
        // The crash closed the createIndexes connection.
        awaitCreateIndex({checkExitSuccess: false});

        // The node restarts as a secondary and keeps retrying its vote, which reaches itself once
        // the node is primary.
        jsTest.log.info("Electing the node at the limit", {buildUUID});
        this.rst.stepUp(primary, {awaitReplicationBeforeStepUp: false});

        for (const node of this.rst.nodes) {
            node.setSecondaryOk();
            IndexBuildTest.assertIndexesSoon(testColl(node), 1, ["_id_"]);
        }
        assertExceededLimit(primary, buildUUID);
        assert.eq(0, pausedAttempts(primary));
    });

    it("clean restart keeps earlier crashes counted", () => {
        this.rst = startReplSet([
            {},
            {
                rsConfig: {priority: 0},
                setParameter: {maxIndexBuildAttemptsAfterUncleanShutdown: kLimit},
            },
        ]);
        const primary = this.rst.getPrimary();

        // Keep the build unfinished across the secondary's restarts.
        IndexBuildTest.pauseIndexBuilds(primary);
        const {buildUUID, awaitCreateIndex} = startBuildAndPauseScan(primary, this.rst.nodes[1], {
            commitQuorum: 2,
        });
        const nodeId = crashDuringPausedScan(this.rst, this.rst.nodes[1], buildUUID);

        const params = {maxIndexBuildAttemptsAfterUncleanShutdown: kLimit};
        let secondary = this.rst.start(nodeId, {setParameter: params}, /*restart=*/ true);
        secondary.setSecondaryOk();
        // Startup counted the crash.
        assert.eq(1, attemptsAfterUncleanShutdown(secondary, buildUUID));

        jsTest.log.info("Restarting the secondary cleanly", {buildUUID});
        this.rst.restart(nodeId, {setParameter: params});
        secondary = this.rst.nodes[nodeId];
        secondary.setSecondaryOk();
        // The attempt that ended in a clean shutdown didn't crash, so the count stays at 1, and the
        // node keeps building instead of voting to abort.
        IndexBuildTest.resumeIndexBuilds(primary);
        awaitCreateIndex();
        IndexBuildTest.assertIndexesSoon(testColl(secondary), 2, ["_id_", "a_1"]);
        assert.eq(
            0,
            assert.commandWorked(secondary.adminCommand({serverStatus: 1})).indexBuilds
                .exceededAttemptLimit,
        );
    });

    it("secondary skips scan when the primary already aborted the build", () => {
        this.rst = startReplSet([
            {},
            // Lets the abort commit while the crashing node isn't replicating.
            {rsConfig: {priority: 0}},
            {
                rsConfig: {priority: 0},
                setParameter: {maxIndexBuildAttemptsAfterUncleanShutdown: kLimit},
            },
        ]);
        const primary = this.rst.getPrimary();

        // The default commit quorum includes the crashing node, so the primary can't commit.
        const {buildUUID, awaitCreateIndex} = startBuildAndPauseScan(primary, this.rst.nodes[2], {
            expectedFailures: [ErrorCodes.IndexBuildAborted],
        });
        // Keep the abortIndexBuild entry from reaching the crashing node.
        const stopReplication = {"failpoint.stopReplProducer": tojson({mode: "alwaysOn"})};
        let node = crashUntilLimit(this.rst, this.rst.nodes[2], buildUUID, {
            pauseAtLimit: true,
            setParameter: stopReplication,
        });
        assertExceededLimit(node, buildUUID);
        awaitCreateIndex();
        IndexBuildTest.assertIndexesSoon(testColl(primary), 1, ["_id_"]);

        jsTest.log.info("Restarting the node at the limit before the abort reaches it", {
            buildUUID,
        });
        const nodeId = this.rst.getNodeId(node);
        kill(this.rst, nodeId);
        node = this.rst.start(
            nodeId,
            {setParameter: pausingParams(stopReplication)},
            /*restart=*/ true,
        );
        node.setSecondaryOk();

        // The primary no longer has the build, so it refuses the abort vote, and the node waits
        // for the abort instead of building.
        assert.eq(kLimit + 2, attemptsAfterUncleanShutdown(node, buildUUID));
        assert.soon(
            () =>
                assert.commandWorked(node.adminCommand({serverStatus: 1})).indexBuilds
                    .exceededAttemptLimit === 1,
        );
        restartServerReplication(node);
        IndexBuildTest.assertIndexesSoon(testColl(node), 1, ["_id_"]);
        assert.eq(0, pausedAttempts(node));
    });

    it("secondary finishes the build when the primary has committed it", () => {
        this.rst = startReplSet([
            {},
            {},
            {
                // A non-voting member isn't part of the default commit quorum, so the primary
                // commits without it.
                rsConfig: {priority: 0, votes: 0},
                setParameter: {maxIndexBuildAttemptsAfterUncleanShutdown: kLimit},
            },
        ]);
        const primary = this.rst.getPrimary();

        IndexBuildTest.pauseIndexBuilds(primary);
        const {buildUUID, awaitCreateIndex} = startBuildAndPauseScan(primary, this.rst.nodes[2]);
        // Keep the commitIndexBuild entry from reaching the node until after it votes.
        stopServerReplication(this.rst.nodes[2]);
        IndexBuildTest.resumeIndexBuilds(primary);
        awaitCreateIndex();

        // Stop hanging at the limit, simulating a crash cause that went away.
        const node = crashUntilLimit(this.rst, this.rst.nodes[2], buildUUID, {
            pauseAtLimit: false,
            setParameter: {"failpoint.stopReplProducer": tojson({mode: "alwaysOn"})},
        });
        assertExceededLimit(node, buildUUID);
        // The primary already unregistered the committed build, so it refuses the vote, and the
        // node waits for the commit instead of building.
        IndexBuildTest.assertIndexesSoon(testColl(node), 2, ["_id_"], ["a_1"]);
        // Waiting for the primary's decision doesn't count as another attempt.
        assert.eq(kLimit + 1, attemptsAfterUncleanShutdown(node, buildUUID));

        restartServerReplication(node);
        IndexBuildTest.assertIndexesSoon(testColl(node), 2, ["_id_", "a_1"]);
    });
});
