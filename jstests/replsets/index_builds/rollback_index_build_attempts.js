/**
 * Tests the behavior of the index build attempt ledger when rollback undoes a build's start, commit
 * or abort. The counts in 'local.system.indexBuilds' aren't rolled back.
 *
 * @tags: [
 *   # Primary-driven index builds don't count attempts.
 *   primary_driven_index_builds_incompatible,
 *   requires_mongobridge,
 *   requires_persistence,
 *   requires_rollbacks,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

function testColl(node, collName) {
    return node.getDB("test").getCollection(collName);
}

function attemptsAfterUncleanShutdown(node, buildUUID) {
    return IndexBuildTest.findIndexBuildAttempts(node, buildUUID)?.numAttemptsAfterUncleanShutdown;
}

// Kills the build on 'node', and lets the abort run.
function killBuild(node, collName) {
    const db = node.getDB("test");
    assert.commandWorked(db.killOp(IndexBuildTest.getIndexBuildOpId(db, collName, "a_1")));
    IndexBuildTest.resumeIndexBuilds(node);
}

describe("Index build attempt counts across rollback", function () {
    before(() => {
        this.testData = {
            rollbackShutdowns: TestData.rollbackShutdowns,
            allowUncleanShutdowns: TestData.allowUncleanShutdowns,
            skipEnforceFastCountOnValidate: TestData.skipEnforceFastCountOnValidate,
        };
        // RollbackTest.restartNode only kills nodes when these are set.
        TestData.rollbackShutdowns = true;
        TestData.allowUncleanShutdowns = true;
        // This test crashes nodes, which may cause inaccurate fast counts.
        TestData.skipEnforceFastCountOnValidate = true;
        this.rollbackTest = new RollbackTest(jsTestName(), undefined, {syncdelay: 1});
        this.numCollections = 0;
    });

    after(() => {
        this.rollbackTest.stop();
        Object.assign(TestData, this.testData);
    });

    // Returns the name of a new collection with a document on every node.
    const makeCollection = () => {
        const collName = `${jsTestName()}_${this.numCollections++}`;
        assert.commandWorked(testColl(this.rollbackTest.getPrimary(), collName).insert({a: 1}));
        this.rollbackTest.awaitReplication();
        return collName;
    };

    // Starts a build on 'primary' and pauses its scan, after the build is in the ledger. Returns
    // the build UUID and a function that waits for createIndexes to finish.
    const startPausedBuild = (primary, collName, expectedFailures = []) => {
        IndexBuildTest.pauseIndexBuilds(primary);
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(
            primary,
            testColl(primary, collName).getFullName(),
            {a: 1},
            {},
            expectedFailures,
            // The node that rolls back has to finish builds on its own.
            /*commitQuorum=*/ 1,
        );
        IndexBuildTest.waitForIndexBuildToScanCollection(primary.getDB("test"), collName, "a_1");
        const buildUUID = extractUUIDFromObject(
            IndexBuildTest.assertIndexesSoon(testColl(primary, collName), 2, ["_id_"], ["a_1"], {
                includeBuildUUIDs: true,
            })["a_1"].buildUUID,
        );
        return {buildUUID, awaitCreateIndex};
    };

    // Starts a build that rollback can't undo, then kills the primary mid-build so it restarts with
    // one crash counted. The build stays paused on both data-bearing nodes. Returns the build UUID.
    const startBuildAndCrashPrimary = (collName) => {
        const primary = this.rollbackTest.getPrimary();
        // Keep the build unfinished on the node that becomes primary when the other rolls back.
        IndexBuildTest.pauseIndexBuilds(this.rollbackTest.getSecondary());
        const {buildUUID, awaitCreateIndex} = startPausedBuild(primary, collName);
        // Wait for startIndexBuild to be in a stable checkpoint.
        IndexBuildTest.assertIndexesStartedDurableSoon(testColl(primary, collName), "a_1");

        jsTest.log.info("Killing the primary during the build", {buildUUID});
        this.rollbackTest.restartNode(
            this.rollbackTest.getTestFixture().getNodeId(primary),
            /*SIGKILL*/ 9,
            {setParameter: {"failpoint.hangAfterStartingIndexBuild": tojson({mode: "alwaysOn"})}},
        );
        // The crash closed the createIndexes connection.
        awaitCreateIndex({checkExitSuccess: false});
        assert.eq(1, attemptsAfterUncleanShutdown(this.rollbackTest.getPrimary(), buildUUID));
        return buildUUID;
    };

    // Finishes the rollback that transitionToRollbackOperations() started.
    const rollBack = () => {
        this.rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
        this.rollbackTest.transitionToSyncSourceOperationsDuringRollback();
        this.rollbackTest.transitionToSteadyStateOperations({skipDataConsistencyChecks: true});
    };

    // Lets both data-bearing nodes finish the build, and checks that the commit removed its counts.
    const finishBuild = (collName, buildUUID) => {
        const nodes = [this.rollbackTest.getPrimary(), this.rollbackTest.getSecondary()];
        for (const node of nodes) {
            IndexBuildTest.resumeIndexBuilds(node);
        }
        for (const node of nodes) {
            node.setSecondaryOk();
            IndexBuildTest.assertIndexesSoon(testColl(node, collName), 2, ["_id_", "a_1"]);
            IndexBuildTest.assertIndexBuildAttemptsRemovedSoon(node, buildUUID);
        }
    };

    // Rolls back a commit or abort on 'rollbackNode'. The build restarts, but the index build's
    // attempt counts have already been deleted, so we reset and count from 0.
    // TODO SERVER-138116: Keep the attempt count when rollback undoes a commit or abort, and expect
    // 1 here.
    const rollBackAndAssertCountResets = (rollbackNode, collName, buildUUID) => {
        rollBack();
        rollbackNode.setSecondaryOk();
        assert.soon(() => attemptsAfterUncleanShutdown(rollbackNode, buildUUID) === 0);
        finishBuild(collName, buildUUID);
    };

    // Rolls back a build's start, and checks that neither the build nor its count is left.
    const rollBackAndAssertBuildGone = (rollbackNode, collName, buildUUID) => {
        rollBack();
        rollbackNode.setSecondaryOk();
        IndexBuildTest.assertIndexesSoon(testColl(rollbackNode, collName), 1, ["_id_"]);
        IndexBuildTest.assertIndexBuildAttemptsRemovedSoon(rollbackNode, buildUUID);
    };

    it("rolling back startIndexBuild removes the count", () => {
        const collName = makeCollection();
        const rollbackNode = this.rollbackTest.transitionToRollbackOperations();
        const {buildUUID, awaitCreateIndex} = startPausedBuild(rollbackNode, collName);
        assert.eq(0, attemptsAfterUncleanShutdown(rollbackNode, buildUUID));

        // Rollback steps the node down, which stops the build.
        IndexBuildTest.resumeIndexBuilds(rollbackNode);
        rollBackAndAssertBuildGone(rollbackNode, collName, buildUUID);
        awaitCreateIndex({checkExitSuccess: false});
    });

    it("rolling back to mid-build keeps the count", () => {
        const collName = makeCollection();
        const buildUUID = startBuildAndCrashPrimary(collName);

        const rollbackNode = this.rollbackTest.transitionToRollbackOperations();
        rollBack();
        // Rollback restarts the build in the same process, which doesn't count another attempt.
        rollbackNode.setSecondaryOk();
        IndexBuildTest.waitForIndexBuildToScanCollection(
            rollbackNode.getDB("test"),
            collName,
            "a_1",
        );
        assert.eq(1, attemptsAfterUncleanShutdown(rollbackNode, buildUUID));

        finishBuild(collName, buildUUID);
    });

    it("rolling back commitIndexBuild resets the count", () => {
        const collName = makeCollection();
        const buildUUID = startBuildAndCrashPrimary(collName);

        // Commit only on the node that rolls back. The commit deletes its count.
        const rollbackNode = this.rollbackTest.transitionToRollbackOperations();
        IndexBuildTest.resumeIndexBuilds(rollbackNode);
        IndexBuildTest.assertIndexesSoon(testColl(rollbackNode, collName), 2, ["_id_", "a_1"]);
        IndexBuildTest.assertIndexBuildAttemptsRemovedSoon(rollbackNode, buildUUID);

        rollBackAndAssertCountResets(rollbackNode, collName, buildUUID);
    });

    it("rolling back startIndexBuild and commitIndexBuild removes the ledger entry", () => {
        const collName = makeCollection();
        const rollbackNode = this.rollbackTest.transitionToRollbackOperations();
        const {buildUUID, awaitCreateIndex} = startPausedBuild(rollbackNode, collName);
        IndexBuildTest.resumeIndexBuilds(rollbackNode);
        awaitCreateIndex();
        IndexBuildTest.assertIndexBuildAttemptsRemovedSoon(rollbackNode, buildUUID);

        rollBackAndAssertBuildGone(rollbackNode, collName, buildUUID);
    });

    it("rolling back abortIndexBuild resets the count", () => {
        const collName = makeCollection();
        const buildUUID = startBuildAndCrashPrimary(collName);

        // Abort only on the node that rolls back. The abort deletes its count.
        const rollbackNode = this.rollbackTest.transitionToRollbackOperations();
        killBuild(rollbackNode, collName);
        IndexBuildTest.assertIndexesSoon(testColl(rollbackNode, collName), 1, ["_id_"]);
        IndexBuildTest.assertIndexBuildAttemptsRemovedSoon(rollbackNode, buildUUID);

        rollBackAndAssertCountResets(rollbackNode, collName, buildUUID);
    });

    it("rolling back startIndexBuild and abortIndexBuild removes the ledger entry", () => {
        const collName = makeCollection();
        const rollbackNode = this.rollbackTest.transitionToRollbackOperations();
        const {buildUUID, awaitCreateIndex} = startPausedBuild(rollbackNode, collName, [
            ErrorCodes.Interrupted,
        ]);
        killBuild(rollbackNode, collName);
        awaitCreateIndex();
        IndexBuildTest.assertIndexBuildAttemptsRemovedSoon(rollbackNode, buildUUID);

        rollBackAndAssertBuildGone(rollbackNode, collName, buildUUID);
    });
});
