load("jstests/noPassthrough/libs/index_build.js");
load('jstests/replsets/libs/rollback_test.js');

const RollbackResumableIndexBuildTest = class {
    /**
     * Runs a resumable index build rollback test. The phase that the index build will be in when
     * rollback starts is specified by rollbackStartFailPointName. The phase that the index build
     * will resume from after rollback completes is specified by rollbackEndFailPointName. If
     * either of these points is in the drain writes phase, documents to insert into the side
     * writes table must be specified by sideWrites. locksYieldedFailPointName specifies a point
     * during the index build between rollbackEndFailPointName and rollbackStartFailPointName at
     * which its locks are yielded. Documents specified by insertsToBeRolledBack are inserted after
     * transitioning to rollback operations and will be rolled back.
     */
    static run(rollbackTest,
               dbName,
               collName,
               indexSpec,
               rollbackStartFailPointName,
               rollbackStartFailPointData,
               rollbackEndFailPointName,
               rollbackEndFailPointData,
               locksYieldedFailPointName,
               insertsToBeRolledBack,
               sideWrites = []) {
        const originalPrimary = rollbackTest.getPrimary();

        if (!ResumableIndexBuildTest.resumableIndexBuildsEnabled(originalPrimary)) {
            jsTestLog("Skipping test because resumable index builds are not enabled");
            return;
        }

        rollbackTest.awaitLastOpCommitted();

        // Set internalQueryExecYieldIterations to 0 and maxIndexBuildDrainBatchSize to 1 so that
        // the index build is guaranteed to yield its locks between the rollback end and start
        // failpoints.
        assert.commandWorked(
            originalPrimary.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 0}));
        assert.commandWorked(
            originalPrimary.adminCommand({setParameter: 1, maxIndexBuildDrainBatchSize: 1}));

        const coll = originalPrimary.getDB(dbName).getCollection(collName);
        const indexName = "rollback_resumable_index_build";

        const rollbackEndFp =
            configureFailPoint(originalPrimary, rollbackEndFailPointName, rollbackEndFailPointData);
        const rollbackStartFp = configureFailPoint(
            originalPrimary, rollbackStartFailPointName, rollbackStartFailPointData);

        const awaitCreateIndex = ResumableIndexBuildTest.createIndexWithSideWrites(
            rollbackTest, function(collName, indexSpec, indexName) {
                assert.commandFailedWithCode(
                    db.getCollection(collName).createIndex(indexSpec, {name: indexName}),
                    ErrorCodes.InterruptedDueToReplStateChange);
            }, coll, indexSpec, indexName, sideWrites);

        // Wait until we've completed the last operation that won't be rolled back so that we can
        // begin the operations that will be rolled back.
        rollbackEndFp.wait();

        const buildUUID = extractUUIDFromObject(
            IndexBuildTest
                .assertIndexes(coll, 2, ["_id_"], [indexName], {includeBuildUUIDs: true})[indexName]
                .buildUUID);

        rollbackTest.transitionToRollbackOperations();

        assert.commandWorked(coll.insert(insertsToBeRolledBack));

        // Move the index build forward to a point at which its locks are yielded. This allows the
        // primary to step down during the call to transitionToSyncSourceOperationsBeforeRollback()
        // below.
        const locksYieldedFp = configureFailPoint(
            originalPrimary, locksYieldedFailPointName, {namespace: coll.getFullName()});
        rollbackEndFp.off();
        locksYieldedFp.wait();

        rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

        // The index creation will report as having failed due to InterruptedDueToReplStateChange,
        // but it is still building in the background.
        awaitCreateIndex();

        // Wait until the index build reaches the desired starting point so that we can start the
        // rollback.
        locksYieldedFp.off();
        rollbackStartFp.wait();

        // We ignore the return value here because the node will go into rollback immediately upon
        // the failpoint being disabled, causing the configureFailPoint command to appear as if it
        // failed to run due to a network error despite successfully disabling the failpoint.
        startParallelShell(
            funWithArgs(function(rollbackStartFailPointName) {
                // Wait until we reach rollback state and then disable the failpoint
                // so that the index build can be interrupted for rollback.
                checkLog.containsJson(db.getMongo(), 21593);
                db.adminCommand({configureFailPoint: rollbackStartFailPointName, mode: "off"});
            }, rollbackStartFailPointName), originalPrimary.port);

        rollbackTest.transitionToSyncSourceOperationsDuringRollback();

        // Ensure that the index build was interrupted for rollback.
        checkLog.containsJson(originalPrimary, 20347, {
            buildUUID: function(uuid) {
                return uuid["uuid"]["$uuid"] === buildUUID;
            }
        });

        rollbackTest.transitionToSteadyStateOperations();

        // Ensure that the index build completed after rollback.
        ResumableIndexBuildTest.assertCompleted(
            originalPrimary, coll, buildUUID, 2, ["_id_", indexName]);

        assert.commandWorked(
            rollbackTest.getPrimary().getDB(dbName).getCollection(collName).dropIndex(indexName));
    }

    /**
     * Runs the resumable index build rollback test in the case where rollback begins after the
     * index build has already completed. The point during the index build to roll back to is
     * specified by rollbackEndFailPointName. If this point is in the drain writes phase, documents
     * to insert into the side writes table must be specified by sideWrites. Documents specified by
     * insertsToBeRolledBack are inserted after transitioning to rollback operations and will be
     * rolled back.
     */
    static runIndexBuildComplete(rollbackTest,
                                 dbName,
                                 collName,
                                 indexSpec,
                                 rollbackEndFailPointName,
                                 rollbackEndFailPointData,
                                 insertsToBeRolledBack,
                                 sideWrites = []) {
        const originalPrimary = rollbackTest.getPrimary();

        if (!ResumableIndexBuildTest.resumableIndexBuildsEnabled(originalPrimary)) {
            jsTestLog("Skipping test because resumable index builds are not enabled");
            return;
        }

        rollbackTest.awaitLastOpCommitted();

        const coll = originalPrimary.getDB(dbName).getCollection(collName);
        const indexName = "rollback_resumable_index_build";

        const rollbackEndFp =
            configureFailPoint(originalPrimary, rollbackEndFailPointName, rollbackEndFailPointData);

        const awaitCreateIndex = ResumableIndexBuildTest.createIndexWithSideWrites(
            rollbackTest, function(collName, indexSpec, indexName) {
                assert.commandWorked(db.runCommand({
                    createIndexes: collName,
                    indexes: [{key: indexSpec, name: indexName}],
                    // Commit quorum is disabled so that the index build can
                    // complete while the primary is isolated and will roll back.
                    commitQuorum: 0
                }));
            }, coll, indexSpec, indexName, sideWrites);

        // Wait until we reach the desired ending point so that we can begin the operations that
        // will be rolled back.
        rollbackEndFp.wait();

        const buildUUID = extractUUIDFromObject(
            IndexBuildTest
                .assertIndexes(coll, 2, ["_id_"], [indexName], {includeBuildUUIDs: true})[indexName]
                .buildUUID);

        rollbackTest.transitionToRollbackOperations();

        assert.commandWorked(coll.insert(insertsToBeRolledBack));

        // Disable the rollback end failpoint so that the index build can continue and wait for the
        // index build to complete.
        rollbackEndFp.off();
        awaitCreateIndex();

        rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
        rollbackTest.transitionToSyncSourceOperationsDuringRollback();
        rollbackTest.transitionToSteadyStateOperations();

        // Ensure that the index build restarted after rollback.
        checkLog.containsJson(originalPrimary, 20660, {
            buildUUID: function(uuid) {
                return uuid["uuid"]["$uuid"] === buildUUID;
            }
        });

        // Ensure that the index build completed after rollback.
        ResumableIndexBuildTest.assertCompleted(
            originalPrimary, coll, buildUUID, 2, ["_id_", indexName]);

        assert.commandWorked(
            rollbackTest.getPrimary().getDB(dbName).getCollection(collName).dropIndex(indexName));
    }
};