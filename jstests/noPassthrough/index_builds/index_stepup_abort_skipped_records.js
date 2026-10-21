/**
 * When a node becomes primary, it verifies in progress index builds and aborts any which have
 * skipped records that still cause key generation errors.
 *
 * @tags: [
 *   # TODO(SERVER-109702): Evaluate if a primary-driven index build compatible test should be created.
 *   requires_commit_quorum,
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const kIndexName = "geometry_2dsphere";
const kStepUpAbortReason = "Skipped records retry failed on step-up";

describe("step-up check of index builds with skipped records", function () {
    beforeEach(function () {
        this.rst = new ReplSetTest({nodes: 2});
        this.rst.startSet();
        this.rst.initiate();

        const primary = this.rst.getPrimary();
        this.primary = primary;
        const primaryDB = primary.getDB("test");
        this.primaryColl = primaryDB.getCollection("test");

        this.secondary = this.rst.getSecondary();
        const secondaryDB = this.secondary.getDB(primaryDB.getName());
        this.secondaryColl = secondaryDB.getCollection("test");

        // Avoid optimization on empty colls. Invalid 2dsphere key.
        assert.commandWorked(
            this.primaryColl.insert({
                geometry: {
                    type: "Polygon",
                    coordinates: [
                        [
                            [0, 0],
                            [0, 1],
                            [1, 1],
                            [-2, -1],
                            [0, 0],
                        ],
                    ],
                },
            }),
        );

        // Pause the index builds on the primary, using the 'hangAfterInitializingIndexBuild'
        // failpoint.
        this.failpointHangAfterInit = configureFailPoint(
            primaryDB,
            "hangAfterInitializingIndexBuild",
        );

        // Create the index and start the build, the secondary will be stepping up, so the command
        // should fail due to replication state change.
        this.createIdx = IndexBuildTest.startIndexBuild(
            primary,
            this.primaryColl.getFullName(),
            {geometry: "2dsphere"},
            {},
            [ErrorCodes.InterruptedDueToReplStateChange],
            /*commitQuorum: */ 2,
        );

        IndexBuildTest.waitForIndexBuildToStart(
            secondaryDB,
            this.secondaryColl.getName(),
            kIndexName,
        );

        IndexBuildTest.assertIndexesSoon(this.primaryColl, 2, ["_id_"], [kIndexName]);
        IndexBuildTest.assertIndexesSoon(this.secondaryColl, 2, ["_id_"], [kIndexName]);

        // Wait for the secondary's build to reach "waiting for next action", so the skipped record
        // is recorded before the step-up check runs. Otherwise the check may never abort the build.
        checkLog.containsJson(this.secondary, 3856203);
    });

    afterEach(function () {
        this.rst.stopSet();
    });

    it("aborts the build", function () {
        this.rst.stepUp(this.secondary);

        this.createIdx();

        // The new primary should eventually abort the build.
        IndexBuildTest.assertIndexesSoon(this.primaryColl, 1, ["_id_"]);
        IndexBuildTest.assertIndexesSoon(this.secondaryColl, 1, ["_id_"]);

        // Verify failure reason is due to step-up check.
        assert(
            checkLog.checkContainsOnceJsonStringMatch(
                this.secondary,
                4656003,
                "errmsg",
                kStepUpAbortReason,
            ),
            "expected the index build to have been aborted by the step-up skipped records retry",
        );
    });

    // The abort can lose a race with the build being signaled to commit. This is harmless, as the
    // skipped records are retried again when the build commits.
    it("tolerates the abort losing a race with the build being signaled to commit", function () {
        // Hold the step-up task's abort after it has taken its locks but before it checks whether
        // the build can still be aborted.
        const hangAbort = configureFailPoint(
            this.secondary,
            "hangAbortIndexBuildByBuildUUIDAfterLocks",
        );

        // The old primary's vote completes once the new primary has handled 'voteCommitIndexBuild',
        // which is after it has signalled its build to commit.
        const hangAfterVote = configureFailPoint(
            this.primary,
            "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
        );

        this.rst.stepUp(this.secondary);
        hangAbort.wait();

        // Let the old primary's build proceed. It votes to commit, which satisfies the commit
        // quorum on the new primary and signals its build to commit.
        this.failpointHangAfterInit.off();
        hangAfterVote.wait();
        hangAfterVote.off();

        // The abort now finds the build already signaled to commit, so it cannot abort it.
        hangAbort.off();

        // The failed abort is logged, and does not kill the step-up task.
        checkLog.containsJson(this.secondary, 13699000);
        checkLog.containsJson(this.secondary, 7508300);

        this.createIdx();

        // The skipped records are retried when the build commits, so it still fails there rather
        // than committing an index with invalid keys.
        IndexBuildTest.assertIndexesSoon(this.primaryColl, 1, ["_id_"]);
        IndexBuildTest.assertIndexesSoon(this.secondaryColl, 1, ["_id_"]);
        // "Index build: failed", with the error from the skipped record failing key generation.
        checkLog.containsJson(this.secondary, 20649, {
            error: (error) => error.codeName === "GeoKeyExtractionFailed",
        });
        assert(
            !checkLog.checkContainsOnceJsonStringMatch(
                this.secondary,
                4656003,
                "errmsg",
                kStepUpAbortReason,
            ),
            "the step-up check should not have been the one to abort the index build",
        );
    });
});
