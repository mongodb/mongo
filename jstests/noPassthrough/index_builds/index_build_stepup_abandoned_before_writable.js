/**
 * Verifies that the index build step-up async task exits if the step-up it was launched for is
 * abandoned before the node ever becomes writable.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

describe("index build step-up task", function () {
    const dbName = "test";
    const collName = "coll";

    before(function () {
        this.rst = new ReplSetTest({nodes: 3});
        this.rst.startSet();
        this.rst.initiate();
    });

    after(function () {
        this.rst.stopSet();
    });

    it("exits when a higher term abandons the step-up before the node is writable", function () {
        const rst = this.rst;
        const primary = rst.getPrimary();
        const primaryColl = primary.getDB(dbName).getCollection(collName);

        assert.commandWorked(primaryColl.insert({a: 1}));
        rst.awaitReplication();

        const [newPrimary, otherSecondary] = rst.getSecondaries();

        // With two-phase index builds, keep a build in progress on every node so the step-up task
        // has a build to check.
        const twoPhase = !FeatureFlagUtil.isPresentAndEnabled(primary, "PrimaryDrivenIndexBuilds");
        let buildFps = [];
        let awaitIndexBuild = () => {};
        if (twoPhase) {
            buildFps = rst.nodes.map((node) =>
                configureFailPoint(node, "hangAfterIndexBuildDumpsInsertsFromBulk"),
            );
            awaitIndexBuild = IndexBuildTest.startIndexBuild(
                primary,
                primaryColl.getFullName(),
                {a: 1},
                null,
                [ErrorCodes.InterruptedDueToReplStateChange],
            );
            buildFps.forEach((fp) => fp.wait());
        }

        // Hang the step-up after its last check that it may become primary, but before the index
        // build step-up task is launched. Also hold the step-up task back until the step-up has
        // been abandoned. replSetStepUp may wait for the step-up to finish, so run it in a parallel
        // shell. Whether it reports success depends on timing.
        const taskFp = configureFailPoint(
            newPrimary,
            "hangOnStepUpAsyncTaskBeforeWaitingForWritable",
        );
        const drainFp = configureFailPoint(newPrimary, "hangAfterReconfigOnDrainComplete");
        const awaitStepUp = startParallelShell(() => {
            db.adminCommand({replSetStepUp: 1});
        }, newPrimary.port);
        drainFp.wait();

        // A higher term abandons the step-up. The stepdown it triggers waits for the step-up to
        // finish, so the vote request does not return until the fail point is released.
        const {term} = assert.commandWorked(newPrimary.adminCommand({replSetGetStatus: 1}));
        const config = rst.getReplSetConfigFromNode(newPrimary.nodeId);
        const awaitVoteRequest = startParallelShell(
            funWithArgs(
                function (setName, term, candidateIndex, configVersion, configTerm) {
                    // The vote is not granted, we only need the node to learn of the new term.
                    assert.commandWorked(
                        db.adminCommand({
                            replSetRequestVotes: 1,
                            setName: setName,
                            dryRun: false,
                            term: NumberLong(term),
                            candidateIndex: candidateIndex,
                            configVersion: configVersion,
                            configTerm: NumberLong(configTerm),
                            lastAppliedOpTime: {ts: Timestamp(0, 0), t: NumberLong(-1)},
                        }),
                    );
                },
                rst.name,
                term + 1,
                otherSecondary.nodeId,
                config.version,
                config.term,
            ),
            newPrimary.port,
        );

        // "Stepping down from primary, because a new term has begun"
        checkLog.containsJson(newPrimary, 21402);

        // The step-up still launches the step-up task, but never makes the node writable.
        drainFp.off();
        taskFp.wait();
        rst.awaitSecondaryNodes(null, [newPrimary]);
        awaitVoteRequest();
        awaitStepUp();

        // The node never became writable, and it never will in this term. The step-up task must
        // notice that rather than retry forever.
        // "Finished performing asynchronous step-up checks on index builds"
        taskFp.off();
        checkLog.containsJson(newPrimary, 7508300);

        // The next step-up does not wait on the previous step-up task. There is no primary to
        // replicate from first.
        rst.stepUp(newPrimary, {awaitReplicationBeforeStepUp: false});

        if (twoPhase) {
            buildFps.forEach((fp) => fp.off());
            awaitIndexBuild();

            rst.awaitReplication();
            for (const node of rst.nodes) {
                IndexBuildTest.assertIndexesSoon(node.getDB(dbName).getCollection(collName), 2, [
                    "_id_",
                    "a_1",
                ]);
            }
        }
    });
});
