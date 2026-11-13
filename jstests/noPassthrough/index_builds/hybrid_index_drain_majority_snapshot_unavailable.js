/**
 * Tests majority snapshot loss during side write draining. The build must tolerate snapshot loss
 * before and after commit readiness, and retry draining or complete through the final untimestamped
 * drain.
 *
 * @tags: [
 *   primary_driven_index_builds_incompatible,
 *   requires_index_build_resumability,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

describe("majority snapshot unavailable during index build drains", function () {
    let rst, primary, secondary, testDB, secondaryDB;
    let wakeId = 0;

    before(function () {
        rst = new ReplSetTest({
            nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {arbiterOnly: true}}],
            nodeOptions: {
                setParameter: {
                    featureFlagPrimaryDrivenIndexBuilds: false,
                },
            },
        });
        rst.startSet();
        rst.initiate();
        primary = rst.getPrimary();
        secondary = rst.getSecondary();
        testDB = primary.getDB(jsTestName());
        secondaryDB = secondary.getDB(testDB.getName());
    });

    after(function () {
        rst.stopSet();
    });

    function getBuildInfo(build) {
        const indexes = assert.commandWorked(
            secondaryDB.runCommand({
                listIndexes: build.coll.getName(),
                includeIndexBuildInfo: true,
            }),
        ).cursor.firstBatch;
        const index = indexes.find((index) => index.spec.name === "a_1");
        assert(index && index.indexBuildInfo, "Expected an unfinished index", {indexes});
        return index.indexBuildInfo;
    }

    function startPausedBuild(collName, secondaryPauseName) {
        const build = {
            coll: testDB.getCollection(collName),
            secondaryColl: secondaryDB.getCollection(collName),
        };
        assert.commandWorked(
            testDB.runCommand({
                insert: collName,
                documents: [
                    {_id: 0, a: 0, b: 0},
                    {_id: 1, a: 1, b: 1},
                ],
                writeConcern: {w: "majority"},
            }),
        );
        // Pause the primary before its vote so it cannot decide to commit yet.
        build.primaryPause = configureFailPoint(primary, "hangAfterIndexBuildFirstDrain");
        build.secondaryPause = configureFailPoint(secondary, secondaryPauseName);
        build.awaitBuild = startParallelShell(
            funWithArgs(
                function (dbName, collName) {
                    const result = db.getSiblingDB(dbName).runCommand({
                        createIndexes: collName,
                        indexes: [
                            {key: {a: 1}, name: "a_1"},
                            {key: {b: 1}, name: "b_1"},
                        ],
                        commitQuorum: "votingMembers",
                        // Return without waiting for the paused secondary.
                        writeConcern: {w: 1},
                    });
                    assert.commandWorked(result);
                },
                testDB.getName(),
                collName,
            ),
            primary.port,
        );
        build.primaryPause.wait();
        build.secondaryPause.wait();
        assert.eq(true, getBuildInfo(build).resumable);
        addSideWrites(build);
        return build;
    }

    function addSideWrites(build) {
        // Add writes after scanning so completion must account for side writes.
        assert.commandWorked(
            testDB.runCommand({
                insert: build.coll.getName(),
                documents: [{_id: 2, a: [2, 3], b: 2}],
                writeConcern: {w: "majority"},
            }),
        );
        assert.commandWorked(
            testDB.runCommand({
                update: build.coll.getName(),
                updates: [{q: {_id: 0}, u: {$set: {a: 10, b: 10}}}],
                writeConcern: {w: "majority"},
            }),
        );
        assert.commandWorked(
            testDB.runCommand({
                delete: build.coll.getName(),
                deletes: [{q: {_id: 1}, limit: 1}],
                writeConcern: {w: "majority"},
            }),
        );
        rst.awaitReplication();
    }

    function loseMajoritySnapshot(build) {
        // Prevent publication so the forced reconfig leaves no snapshot.
        build.disableSnapshots = configureFailPoint(secondary, "disableSnapshotting");
        const config = assert.commandWorked(secondary.adminCommand({replSetGetConfig: 1})).config;
        delete config.term;
        config.members.find((member) => member.host === secondary.host).tags = {
            snapshotLoss: build.coll.getName(),
        };
        assert.commandWorked(secondary.adminCommand({replSetReconfig: config, force: true}));
        assertMajoritySnapshotUnavailable();
    }

    function restoreMajoritySnapshot(build) {
        build.disableSnapshots.off();
        // New replication progress triggers snapshot publication after it is reenabled.
        assert.commandWorked(
            testDB.runCommand({
                insert: "snapshotWake",
                documents: [{_id: ++wakeId}],
                writeConcern: {w: 1},
            }),
        );
        rst.awaitLastOpCommitted();
    }

    function assertMajoritySnapshotUnavailable() {
        const status = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
        assert.eq(
            undefined,
            status.optimes.readConcernMajorityOpTime,
            "Expected no majority snapshot",
            {
                optimes: status.optimes,
            },
        );
    }

    function getDrainAttempts() {
        return assert.commandWorked(secondary.adminCommand({serverStatus: 1})).indexBuilds.phases
            .drainSideWritesTable;
    }

    function waitForRepeatedDrainAttempts(initialAttempts) {
        // A second attempt means the first returned while the snapshot remained unavailable.
        assert.soon(
            () => getDrainAttempts() >= initialAttempts + 2,
            "Expected repeated drain attempts while waiting for the commit decision",
        );
        assertMajoritySnapshotUnavailable();
    }

    function waitForPreReadinessSnapshotWait(build) {
        const opid = getBuildInfo(build).opid;
        const initialAttempts = getDrainAttempts();
        build.secondaryPause.off();
        assert.soon(() => {
            if (getDrainAttempts() <= initialAttempts) {
                return false;
            }
            const [op] = secondaryDB
                .getSiblingDB("admin")
                .aggregate([
                    {$currentOp: {allUsers: true, idleConnections: true}},
                    {$match: {opid}},
                ])
                .toArray();
            return op && Object.keys(op.locks || {}).length === 0;
        }, "Expected a pre-readiness wait for snapshot recovery");
        assertMajoritySnapshotUnavailable();
    }

    function assertCommittedIndexContents(build) {
        const coll = build.secondaryColl;
        IndexBuildTest.assertIndexes(coll, 3, ["_id_", "a_1", "b_1"]);
        const expected = [
            {_id: 0, a: 10, b: 10},
            {_id: 2, a: [2, 3], b: 2},
        ];
        for (const index of ["a_1", "b_1"]) {
            assert.docEq(expected, coll.find().hint(index).sort({_id: 1}).toArray());
        }
        assert.eq(
            true,
            assert.commandWorked(secondaryDB.runCommand({validate: coll.getName(), full: true}))
                .valid,
        );
    }

    it("resumes side-write draining after snapshot recovery before voting for commit readiness", function () {
        // Snapshot loss: before voting for commit readiness.
        // Snapshot recovery: before voting for commit readiness.
        // Expected: retry the required drain before voting, then process the commit decision.
        const build = startPausedBuild(
            "beforeVoteRecovery",
            "hangAfterIndexBuildDumpsInsertsFromBulk",
        );
        const beforeVote = configureFailPoint(
            secondary,
            "hangIndexBuildBeforeSignalPrimaryForCommitReadiness",
        );
        loseMajoritySnapshot(build);
        waitForPreReadinessSnapshotWait(build);
        // Observe the required retry so the final drain cannot hide a skipped pre-readiness drain.
        const retryDrain = configureFailPoint(secondary, "hangDuringIndexBuildDrainYield", {
            namespace: build.coll.getFullName(),
        });
        restoreMajoritySnapshot(build);
        retryDrain.wait();
        retryDrain.off();
        beforeVote.wait();
        beforeVote.off();

        build.primaryPause.off();
        build.awaitBuild();
        rst.awaitReplication();
        assertCommittedIndexContents(build);
        assert.commandWorked(testDB.runCommand({drop: build.coll.getName()}));
    });

    it("resumes side-write draining after voting for commit readiness when the snapshot recovers before the commit decision", function () {
        // Snapshot loss: after voting for commit readiness, before the commit decision.
        // Snapshot recovery: after voting for commit readiness, before receiving the commit decision.
        // Expected: resume side-write draining before the commit decision, then finish the build.
        const build = startPausedBuild(
            "snapshotRecovery",
            "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
        );
        loseMajoritySnapshot(build);
        const initialAttempts = getDrainAttempts();
        build.secondaryPause.off();
        waitForRepeatedDrainAttempts(initialAttempts);

        const retryDrain = configureFailPoint(secondary, "hangDuringIndexBuildDrainYield", {
            namespace: build.coll.getFullName(),
        });
        restoreMajoritySnapshot(build);
        retryDrain.wait();
        retryDrain.off();

        build.primaryPause.off();
        build.awaitBuild();
        rst.awaitReplication();
        assertCommittedIndexContents(build);
        assert.commandWorked(testDB.runCommand({drop: build.coll.getName()}));
    });

    it("processes a later commit decision after voting for commit readiness without waiting for snapshot recovery", function () {
        // Snapshot loss: after voting for commit readiness, before the commit decision.
        // Snapshot recovery: after the commit decision is applied and both nodes finish the build.
        // Expected: process the later commit decision despite failed periodic drains,
        // defer the last IX drain, and finish through the final untimestamped drain.
        const build = startPausedBuild(
            "untilCommit",
            "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
        );
        loseMajoritySnapshot(build);
        const initialAttempts = getDrainAttempts();
        build.secondaryPause.off();
        waitForRepeatedDrainAttempts(initialAttempts);

        build.primaryPause.off();
        build.awaitBuild();
        rst.awaitReplication();
        restoreMajoritySnapshot(build);
        assertCommittedIndexContents(build);
        assert.commandWorked(testDB.runCommand({drop: build.coll.getName()}));
    });

    it("processes an already-received commit decision after voting for commit readiness despite snapshot loss", function () {
        // Snapshot loss: after voting for commit readiness and receiving the commit decision.
        // Snapshot recovery: after the commit decision is applied and both nodes finish the build.
        // Expected: defer the last IX drain and finish through the final untimestamped drain.
        const build = startPausedBuild(
            "afterDecision",
            "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
        );
        build.primaryPause.off();
        build.awaitBuild();
        // The ready decision makes the next drain the last IX attempt, not a periodic one.
        assert.soon(
            () => getBuildInfo(build).replicationState.state === "Applying commit oplog entry",
            "Secondary must receive the commit decision before its last IX drain",
        );

        loseMajoritySnapshot(build);
        const initialAttempts = getDrainAttempts();
        build.secondaryPause.off();
        rst.awaitReplication();
        assert.gt(getDrainAttempts(), initialAttempts);
        assertMajoritySnapshotUnavailable();
        restoreMajoritySnapshot(build);
        assertCommittedIndexContents(build);
        assert.commandWorked(testDB.runCommand({drop: build.coll.getName()}));
    });
});
