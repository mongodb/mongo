load("jstests/noPassthrough/libs/index_build.js");
load('jstests/replsets/libs/rollback_test.js');

const RollbackResumableIndexBuildTest = class {
    /**
     * Runs a resumable index build rollback test.
     *
     * 'rollbackStartFailPointName' specifies the phase that the index build will be in when
     *   rollback starts.
     *
     * 'rollbackEndFailPointName' specifies the phase that the index build resume from after
     *   rollback completes.
     *
     * 'locksYieldedFailPointName' specifies a point during the index build between
     *   'rollbackEndFailPointName' and 'rollbackStartFailPointName' at which its locks are yielded.
     *
     * 'expectedResumePhase' is a string which specifies the name of the phase that the index build
     *   is expected to resume in.
     *
     * 'resumeCheck' is an object which contains exactly one of 'numScannedAferResume' and
     *   'skippedPhaseLogID'. The former is used to verify that the index build scanned the
     *   expected number of documents in the collection scan after resuming. The latter is used for
     *   phases which do not perform a collection scan after resuming, to verify that the index
     *   index build did not resume from an earlier phase than expected. The log message must
     *   contain the buildUUID attribute.
     *
     * 'insertsToBeRolledBack' is documents which are inserted after transitioning to rollback
     *   operations and will be rolled back.
     *
     * 'sideWrites' is documents to insert into the side writes table. If either
     *   'rollbackStartFailPointName' or 'rollbackEndFailPointName' the above two are in the drain
     *   writes phase, this is required.
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
               expectedResumePhase,
               resumeCheck,
               insertsToBeRolledBack,
               sideWrites = []) {
        const originalPrimary = rollbackTest.getPrimary();

        if (!ResumableIndexBuildTest.resumableIndexBuildsEnabled(originalPrimary)) {
            jsTestLog("Skipping test because resumable index builds are not enabled");
            return;
        }

        rollbackTest.awaitLastOpCommitted();

        assert.commandWorked(
            originalPrimary.adminCommand({setParameter: 1, logComponentVerbosity: {index: 1}}));

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

        // Clear the log so that we can verify that the index build resumes from the correct phase
        // after rollback.
        assert.commandWorked(originalPrimary.adminCommand({clearLog: "global"}));

        // The parallel shell performs a checkLog, so use this failpoint to synchronize starting
        // the parallel shell with rollback.
        const getLogFp = configureFailPoint(originalPrimary, "hangInGetLog");

        const awaitDisableFailPoint = startParallelShell(
            funWithArgs(function(buildUUID, locksYieldedFailPointName) {
                // Wait for the index build to be aborted for rollback.
                checkLog.containsJson(db.getMongo(), 465611, {
                    buildUUID: function(uuid) {
                        return uuid["uuid"]["$uuid"] === buildUUID;
                    }
                });

                // Disable the failpoint so that the builder thread can exit and rollback can
                // continue.
                assert.commandWorked(
                    db.adminCommand({configureFailPoint: locksYieldedFailPointName, mode: "off"}));
            }, buildUUID, locksYieldedFailPointName), originalPrimary.port);

        getLogFp.wait();
        getLogFp.off();

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

        ResumableIndexBuildTest.checkResume(
            originalPrimary, [buildUUID], [expectedResumePhase], [resumeCheck]);

        ResumableIndexBuildTest.checkIndexes(
            rollbackTest.getTestFixture(), dbName, collName, [indexName], []);
    }
};