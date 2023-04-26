load("jstests/noPassthrough/libs/index_build.js");
load('jstests/replsets/libs/rollback_test.js');

const RollbackResumableIndexBuildTest = class {
    static checkCompletedAndDrop(
        rollbackTest, originalPrimary, dbName, colls, buildUUIDs, indexNames) {
        for (let i = 0; i < colls.length; i++) {
            ResumableIndexBuildTest.assertCompleted(
                originalPrimary, colls[i], [buildUUIDs[i]], indexNames[i]);
            ResumableIndexBuildTest.checkIndexes(
                rollbackTest, dbName, colls[i].getName(), indexNames[i], []);
            assert.commandWorked(
                rollbackTest.getPrimary().getDB(dbName).runCommand({drop: colls[i].getName()}));
        }
    }

    /**
     * Runs a resumable index build rollback test.
     *
     * 'indexSpecs' is a 2d array that specifies all indexes that should be built. The first
     *   dimension indicates separate calls to the createIndexes command, while the second dimension
     *   is for indexes that are built together using one call to createIndexes.
     *
     * 'rollbackStartFailPoints' specifies the phases that the index builds will be in when rollback
     *   starts. It is an array of objects that contain two fields: 'name', which specifies the name
     *   of the failpoint, and exactly one of 'logIdWithBuildUUID' and 'logIdWithIndexName'. The
     *   former is used for failpoints whose log message includes the build UUID, and the latter is
     *   used for failpoints whose log message includes the index name. The array must either
     *   contain one element, in which case that one element will be applied to all index builds
     *   specified above, or it must be exactly the length of 'indexSpecs'.
     *
     * 'rollbackEndFailPoints' specifies the phases that the index builds resume from after rollback
     *   completes. It follows the same roles as 'rollbackStartFailPoints'.
     *
     * 'locksYieldedFailPointNames' is an array of strings with the same length as the first
     *   dimension of 'indexSpecs'. It specifies a point for respective each index build between
     *   'rollbackEndFailPointName' and 'rollbackStartFailPointName' at which its locks are yielded.
     *
     * 'expectedResumePhases' is an array of strings that specify the name of the phases that each
     *   index build is expected to resume in. The array must either contain one string, in which
     *   case all index builds will be expected to resume from that phase, or it must be exactly
     *   the length of 'indexSpecs'.
     *
     * 'resumeChecks' is an array of objects that contain exactly one of 'numScannedAfterResume' and
     *   'skippedPhaseLogID'. The former is used to verify that the index build scanned the expected
     *   number of documents in the collection scan after resuming. The latter is used for phases
     *   which do not perform a collection scan after resuming, to verify that the index build did
     *   not resume from an earlier phase than expected. The log message must contain the buildUUID
     *   attribute.
     *
     * 'insertsToBeRolledBack' is documents which are inserted after transitioning to rollback
     *   operations and will be rolled back.
     *
     * 'sideWrites' is documents to insert into the side writes table. If either
     *   'rollbackStartFailPointName' or 'rollbackEndFailPointName' the above two are in the drain
     *   writes phase, this is required.
     *
     * 'shouldComplete' is a boolean which determines whether the index builds started by the test
     *   fixture should be expected to be completed when this function returns. If false, this
     *   function returns the collections, buildUUIDs, and index names of the index builds started
     *   by the test fixture.
     */
    static run(rollbackTest,
               dbName,
               collNameSuffix,
               docs,
               indexSpecs,
               rollbackStartFailPoints,
               rollbackStartFailPointsIteration,
               rollbackEndFailPoints,
               rollbackEndFailPointsIteration,
               locksYieldedFailPointNames,
               expectedResumePhases,
               resumeChecks,
               insertsToBeRolledBack,
               sideWrites = [],
               {shouldComplete = true} = {}) {
        const originalPrimary = rollbackTest.getPrimary();

        rollbackTest.awaitLastOpCommitted();

        assert.commandWorked(originalPrimary.adminCommand(
            {setParameter: 1, logComponentVerbosity: {index: 1, replication: {heartbeats: 0}}}));

        // Set internalQueryExecYieldIterations to 0, internalIndexBuildBulkLoadYieldIterations to
        // 1, and maxIndexBuildDrainBatchSize to 1 so that the index builds are guaranteed to yield
        // their locks between the rollback end and start failpoints.
        assert.commandWorked(
            originalPrimary.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 0}));
        assert.commandWorked(originalPrimary.adminCommand(
            {setParameter: 1, internalIndexBuildBulkLoadYieldIterations: 1}));
        assert.commandWorked(
            originalPrimary.adminCommand({setParameter: 1, maxIndexBuildDrainBatchSize: 1}));

        configureFailPoint(originalPrimary, "hangBeforeBuildingIndexSecond");

        const indexNames = ResumableIndexBuildTest.generateIndexNames(indexSpecs);
        const awaitCreateIndexes = new Array(indexSpecs.length);
        const buildUUIDs = new Array(indexSpecs.length);
        const colls = new Array(indexSpecs.length);
        for (let i = 0; i < indexSpecs.length; i++) {
            colls[i] = originalPrimary.getDB(dbName).getCollection(jsTestName() + collNameSuffix +
                                                                   "_" + i);
            assert.commandWorked(colls[i].insert(docs));

            awaitCreateIndexes[i] = ResumableIndexBuildTest.createIndexWithSideWrites(
                rollbackTest, function(collName, indexSpecs, indexNames) {
                    load("jstests/noPassthrough/libs/index_build.js");
                    ResumableIndexBuildTest.createIndexesFails(
                        db, collName, indexSpecs, indexNames);
                }, colls[i], indexSpecs[i], indexNames[i], sideWrites);

            buildUUIDs[i] = ResumableIndexBuildTest.generateFailPointsData(
                colls[i],
                [rollbackStartFailPoints[rollbackStartFailPoints.length === 1 ? 0 : i]],
                rollbackStartFailPointsIteration,
                [indexNames[i]])[0];
            ResumableIndexBuildTest.generateFailPointsData(
                colls[i],
                [rollbackEndFailPoints[rollbackEndFailPoints.length === 1 ? 0 : i]],
                rollbackEndFailPointsIteration,
                [indexNames[i]]);
        }

        for (const rollbackStartFailPoint of rollbackStartFailPoints) {
            configureFailPoint(
                originalPrimary, rollbackStartFailPoint.name, rollbackStartFailPoint.data);
        }
        for (const rollbackEndFailPoint of rollbackEndFailPoints) {
            configureFailPoint(
                originalPrimary, rollbackEndFailPoint.name, rollbackEndFailPoint.data);
        }

        // Wait until the index builds have completed their last operations that won't be rolled
        // back so that we can begin the operations that will be rolled back.
        ResumableIndexBuildTest.waitForFailPoints(
            originalPrimary, ["hangBeforeBuildingIndexSecond"], rollbackEndFailPoints);

        rollbackTest.transitionToRollbackOperations();

        // Move the index builds forward to points at which their locks are yielded. This allows the
        // primary to step down during the call to transitionToSyncSourceOperationsBeforeRollback()
        // below.
        const locksYieldedFps = new Array(colls.length);
        for (let i = 0; i < colls.length; i++) {
            assert.commandWorked(colls[i].insert(insertsToBeRolledBack));
            locksYieldedFps[i] = configureFailPoint(originalPrimary,
                                                    locksYieldedFailPointNames[i],
                                                    {namespace: colls[i].getFullName()});
        }
        for (const rollbackEndFailPoint of rollbackEndFailPoints) {
            assert.commandWorked(originalPrimary.adminCommand(
                {configureFailPoint: rollbackEndFailPoint.name, mode: "off"}));
            delete rollbackEndFailPoint.data;
        }
        for (const locksYieldedFp of locksYieldedFps) {
            locksYieldedFp.wait();
        }

        rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

        // The index creation commands will report as having failed due to
        // InterruptedDueToReplStateChange, but they are still building in the background.
        for (const awaitCreateIndex of awaitCreateIndexes) {
            awaitCreateIndex();
        }

        // Wait until the index builds reach the desired starting points for the rollback.
        ResumableIndexBuildTest.waitForFailPoints(
            originalPrimary, locksYieldedFailPointNames, rollbackStartFailPoints);

        // Let the index builds yield their locks so that they can be aborted for rollback.
        for (let i = 0; i < colls.length; i++) {
            locksYieldedFps[i] = configureFailPoint(originalPrimary,
                                                    locksYieldedFailPointNames[i],
                                                    {namespace: colls[i].getFullName()});
        }
        for (const rollbackStartFailPoint of rollbackStartFailPoints) {
            assert.commandWorked(originalPrimary.adminCommand(
                {configureFailPoint: rollbackStartFailPoint.name, mode: "off"}));
            delete rollbackStartFailPoint.data;
        }
        for (const locksYieldedFp of locksYieldedFps) {
            locksYieldedFp.wait();
        }

        // Clear the log so that we can verify that the index builds resume from the correct phases
        // after rollback.
        assert.commandWorked(originalPrimary.adminCommand({clearLog: "global"}));

        // The parallel shells run checkLog, so use this failpoint to synchronize starting the
        // parallel shells with rollback.
        const getLogFp = configureFailPoint(originalPrimary, "hangInGetLog");
        clearRawMongoProgramOutput();

        const awaitDisableFailPoints = new Array(buildUUIDs.length);
        for (let i = 0; i < buildUUIDs.length; i++) {
            awaitDisableFailPoints[i] = startParallelShell(
                funWithArgs(function(buildUUID, locksYieldedFailPointName) {
                    // Wait for the index build to be aborted for rollback.
                    checkLog.containsJson(db.getMongo(), 465611, {
                        buildUUID: function(uuid) {
                            return uuid && uuid["uuid"]["$uuid"] === buildUUID;
                        }
                    });

                    // Disable the failpoint so that the builder thread can exit.
                    assert.commandWorked(db.adminCommand(
                        {configureFailPoint: locksYieldedFailPointName, mode: "off"}));
                }, buildUUIDs[i], locksYieldedFailPointNames[i]), originalPrimary.port);
        }

        // Wait until the parallel shells have all started.
        assert.soon(() => {
            return (rawMongoProgramOutput().match(/"id":5113600/g) || []).length ===
                buildUUIDs.length;
        });
        getLogFp.off();

        rollbackTest.transitionToSyncSourceOperationsDuringRollback();
        for (const awaitDisableFailPoint of awaitDisableFailPoints) {
            awaitDisableFailPoint();
        }

        // Ensure that the index builds were interrupted for rollback.
        for (const buildUUID of buildUUIDs) {
            checkLog.containsJson(originalPrimary, 20347, {
                buildUUID: function(uuid) {
                    return uuid && uuid["uuid"]["$uuid"] === buildUUID;
                }
            });
        }

        rollbackTest.transitionToSteadyStateOperations();

        if (shouldComplete) {
            // Ensure that the index builds completed after rollback.
            RollbackResumableIndexBuildTest.checkCompletedAndDrop(
                rollbackTest, originalPrimary, dbName, colls, buildUUIDs, indexNames);
        }

        ResumableIndexBuildTest.checkResume(
            originalPrimary, buildUUIDs, expectedResumePhases, resumeChecks);

        if (!shouldComplete) {
            return {colls: colls, buildUUIDs: buildUUIDs, indexNames: indexNames};
        }
    }

    static runResumeInterruptedByRollback(
        rollbackTest, dbName, docs, indexSpec, insertsToBeRolledBack, sideWrites = []) {
        const originalPrimary = rollbackTest.getPrimary();

        const fp1 = configureFailPoint(originalPrimary, "hangAfterIndexBuildDumpsInsertsFromBulk");
        const fp2 = configureFailPoint(originalPrimary, "hangAfterIndexBuildFirstDrain");

        const testInfo = RollbackResumableIndexBuildTest.run(
            rollbackTest,
            dbName,
            "",
            docs,
            [[indexSpec]],
            [{
                name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
                logIdWithBuildUUID: 20386
            }],
            1,  // rollbackStartFailPointsIteration
            [{name: "hangAfterSettingUpIndexBuild", logIdWithBuildUUID: 20387}],
            0,  // rollbackEndFailPointsIteration
            ["setYieldAllLocksHang"],
            ["collection scan"],
            [{numScannedAfterResume: docs.length - 1}],
            insertsToBeRolledBack,
            sideWrites,
            {shouldComplete: false});

        // Cycle through the rollback test phases so the original primary becomes primary again.
        rollbackTest.transitionToRollbackOperations();
        rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
        rollbackTest.transitionToSyncSourceOperationsDuringRollback();
        rollbackTest.transitionToSteadyStateOperations();

        // Clear the logs from when the index build was resumed.
        assert.commandWorked(originalPrimary.adminCommand({clearLog: "global"}));

        rollbackTest.transitionToRollbackOperations();

        fp1.wait();
        fp1.off();

        rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

        fp2.wait();
        fp2.off();

        rollbackTest.transitionToSyncSourceOperationsDuringRollback();
        rollbackTest.transitionToSteadyStateOperations();

        // Ensure that the index build restarted, rather than resumed.
        checkLog.containsJson(originalPrimary, 20660, {
            buildUUID: function(uuid) {
                return uuid && uuid["uuid"]["$uuid"] === testInfo.buildUUIDs[0];
            }
        });
        assert(!checkLog.checkContainsOnceJson(originalPrimary, 4841700));

        RollbackResumableIndexBuildTest.checkCompletedAndDrop(rollbackTest,
                                                              originalPrimary,
                                                              dbName,
                                                              testInfo.colls,
                                                              testInfo.buildUUIDs,
                                                              testInfo.indexNames);
    }
};
