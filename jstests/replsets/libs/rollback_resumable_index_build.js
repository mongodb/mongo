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
               rollbackStartFailPointIteration,
               rollbackEndFailPointName,
               rollbackEndFailPointIteration,
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

        const awaitCreateIndex = ResumableIndexBuildTest.createIndexWithSideWrites(
            rollbackTest, function(collName, indexSpec, indexName) {
                assert.commandFailedWithCode(
                    db.getCollection(collName).createIndex(indexSpec, {name: indexName}),
                    ErrorCodes.InterruptedDueToReplStateChange);
            }, coll, indexSpec, indexName, sideWrites, {hangBeforeBuildingIndex: true});

        const buildUUID = extractUUIDFromObject(
            IndexBuildTest
                .assertIndexes(coll, 2, ["_id_"], [indexName], {includeBuildUUIDs: true})[indexName]
                .buildUUID);

        const rollbackEndFp = configureFailPoint(originalPrimary, rollbackEndFailPointName, {
            buildUUIDs: [buildUUID],
            indexNames: [indexName],
            iteration: rollbackEndFailPointIteration
        });
        const rollbackStartFp = configureFailPoint(originalPrimary, rollbackStartFailPointName, {
            buildUUIDs: [buildUUID],
            indexNames: [indexName],
            iteration: rollbackStartFailPointIteration
        });

        assert.commandWorked(originalPrimary.adminCommand(
            {configureFailPoint: "hangBeforeBuildingIndex", mode: "off"}));

        // Wait until we've completed the last operation that won't be rolled back so that we can
        // begin the operations that will be rolled back.
        rollbackEndFp.wait();

        rollbackTest.transitionToRollbackOperations();

        assert.commandWorked(coll.insert(insertsToBeRolledBack));

        // Move the index build forward to a point at which its locks are yielded. This allows the
        // primary to step down during the call to transitionToSyncSourceOperationsBeforeRollback()
        // below.
        let locksYieldedFp = configureFailPoint(
            originalPrimary, locksYieldedFailPointName, {namespace: coll.getFullName()});
        rollbackEndFp.off();
        locksYieldedFp.wait();

        rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

        // The index creation will report as having failed due to InterruptedDueToReplStateChange,
        // but it is still building in the background.
        awaitCreateIndex();

        // Wait until the index build reaches the desired starting point for the rollback.
        locksYieldedFp.off();
        rollbackStartFp.wait();

        // Let the index build yield its locks so that it can be aborted for rollback.
        locksYieldedFp = configureFailPoint(
            originalPrimary, locksYieldedFailPointName, {namespace: coll.getFullName()});
        rollbackStartFp.off();
        locksYieldedFp.wait();

        const awaitDisableFailPoint = startParallelShell(
            // Wait for the index build to be aborted for rollback.
            funWithArgs(
                function(buildUUID, locksYieldedFailPointName) {
                    checkLog.containsJson(db.getMongo(), 465611, {
                        buildUUID: function(uuid) {
                            return uuid["uuid"]["$uuid"] === buildUUID;
                        }
                    });

                    // Disable the failpoint so that the builder thread can exit and rollback can
                    // continue.
                    assert.commandWorked(db.adminCommand(
                        {configureFailPoint: locksYieldedFailPointName, mode: "off"}));
                },
                buildUUID,
                locksYieldedFailPointName),
            originalPrimary.port);

        rollbackTest.transitionToSyncSourceOperationsDuringRollback();
        awaitDisableFailPoint();

        // Ensure that the index build was interrupted for rollback.
        checkLog.containsJson(originalPrimary, 20347, {
            buildUUID: function(uuid) {
                return uuid["uuid"]["$uuid"] === buildUUID;
            }
        });

        rollbackTest.transitionToSteadyStateOperations();

        // Ensure that the index build completed after rollback.
        ResumableIndexBuildTest.assertCompleted(originalPrimary, coll, [buildUUID], [indexName]);

        assert.commandWorked(
            rollbackTest.getPrimary().getDB(dbName).getCollection(collName).dropIndex(indexName));
    }
};