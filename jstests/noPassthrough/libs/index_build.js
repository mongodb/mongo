// Helper functions for testing index builds.

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/uuid_util.js");

var IndexBuildTest = class {
    /**
     * Starts an index build in a separate mongo shell process with given options.
     * Ensures the index build worked or failed with one of the expected failures.
     */
    static startIndexBuild(conn, ns, keyPattern, options, expectedFailures, commitQuorum) {
        options = options || {};
        expectedFailures = expectedFailures || [];
        // The default for the commit quorum parameter to Collection.createIndexes() should be
        // left as undefined if 'commitQuorum' is omitted. This is because we need to differentiate
        // between undefined (which uses the default in the server) and 0 which disables the commit
        // quorum.
        const commitQuorumStr = (commitQuorum === undefined ? '' : ', ' + tojson(commitQuorum));

        if (Array.isArray(keyPattern)) {
            return startParallelShell(
                'const coll = db.getMongo().getCollection("' + ns + '");' +
                    'assert.commandWorkedOrFailedWithCode(coll.createIndexes(' +
                    JSON.stringify(keyPattern) + ', ' + tojson(options) + commitQuorumStr + '), ' +
                    JSON.stringify(expectedFailures) + ');',
                conn.port);
        } else {
            return startParallelShell('const coll = db.getMongo().getCollection("' + ns + '");' +
                                          'assert.commandWorkedOrFailedWithCode(coll.createIndex(' +
                                          tojson(keyPattern) + ', ' + tojson(options) +
                                          commitQuorumStr + '), ' +
                                          JSON.stringify(expectedFailures) + ');',
                                      conn.port);
        }
    }

    /**
     * Returns the op id for the running index build on the provided 'collectionName' and
     * 'indexName', or any index build if either is undefined. Returns -1 if there is no current
     * index build.
     * Accepts optional filter that can be used to customize the db.currentOp() query.
     */
    static getIndexBuildOpId(database, collectionName, indexName, filter) {
        let pipeline = [{$currentOp: {allUsers: true, idleConnections: true}}];
        if (filter) {
            pipeline.push({$match: filter});
        }
        const result = database.getSiblingDB("admin")
                           .aggregate(pipeline, {readConcern: {level: "local"}})
                           .toArray();
        let indexBuildOpId = -1;
        let indexBuildObj = {};
        let indexBuildNamespace = "";

        result.forEach(function(op) {
            if (op.op != 'command') {
                return;
            }
            const cmdBody = op.command;

            if (cmdBody.createIndexes === undefined) {
                return;
            }
            // If no collection is provided, return any index build.
            if (!collectionName || cmdBody.createIndexes === collectionName) {
                cmdBody.indexes.forEach((index) => {
                    if (!indexName || index.name === indexName) {
                        indexBuildOpId = op.opid;
                        indexBuildObj = index;
                        indexBuildNamespace = op.ns;
                    }
                });
            }
        });
        if (indexBuildOpId != -1) {
            jsTestLog("found in progress index build: " + tojson(indexBuildObj) + " on namespace " +
                      indexBuildNamespace + " opid: " + indexBuildOpId);
        }
        return indexBuildOpId;
    }

    /**
     * Wait for index build to start and return its op id.
     * Accepts optional filter that can be used to customize the db.currentOp() query.
     * The filter may be necessary in situations when the index build is delegated to a thread pool
     * managed by the IndexBuildsCoordinator and it is necessary to differentiate between the
     * client connection thread and the IndexBuildsCoordinator thread actively building the index.
     */
    static waitForIndexBuildToStart(database, collectionName, indexName, filter) {
        let opId;
        assert.soon(function() {
            return (opId = IndexBuildTest.getIndexBuildOpId(
                        database, collectionName, indexName, filter)) !== -1;
        }, "Index build operation not found after starting via parallelShell");
        return opId;
    }

    /**
     * Wait for index build to begin its collection scan phase and return its op id.
     */
    static waitForIndexBuildToScanCollection(database, collectionName, indexName) {
        // The collection scan is the only phase of an index build that uses a progress meter.
        // Since the progress meter can be detected in the db.currentOp() output, we will use this
        // information to determine when we are scanning the collection during the index build.
        const filter = {
            progress: {$exists: true},
        };
        return IndexBuildTest.waitForIndexBuildToStart(database, collectionName, indexName, filter);
    }

    /**
     * Wait for all index builds to stop and return its op id.
     */
    static waitForIndexBuildToStop(database, collectionName, indexName) {
        assert.soon(function() {
            return IndexBuildTest.getIndexBuildOpId(database, collectionName, indexName) === -1;
        }, "Index build operations still running after unblocking or killOp");
    }

    /**
     * Checks the db.currentOp() output for the index build with opId.
     *
     * An optional 'onOperationFn' callback accepts an operation to perform any additional checks.
     */
    static assertIndexBuildCurrentOpContents(database, opId, onOperationFn) {
        const inprog = database.currentOp({opid: opId, "$all": true}).inprog;
        assert.eq(1,
                  inprog.length,
                  'unable to find opid ' + opId +
                      ' in currentOp() result: ' + tojson(database.currentOp()));
        const op = inprog[0];
        assert.eq(opId, op.opid, 'db.currentOp() returned wrong index build info: ' + tojson(op));
        if (onOperationFn) {
            onOperationFn(op);
        }
    }

    /**
     * Runs listIndexes command on collection.
     * If 'options' is provided, these will be sent along with the command request.
     * Asserts that all the indexes on this collection fit within the first batch of results.
     * Returns a map of index specs keyed by name.
     */
    static assertIndexes(coll, numIndexes, readyIndexes, notReadyIndexes, options) {
        notReadyIndexes = notReadyIndexes || [];
        options = options || {};

        let res = assert.commandWorked(coll.runCommand("listIndexes", options));
        assert.eq(numIndexes,
                  res.cursor.firstBatch.length,
                  'unexpected number of indexes in collection: ' + tojson(res));

        // First batch contains all the indexes in the collection.
        assert.eq(0, res.cursor.id);

        // A map of index specs keyed by index name.
        const indexMap = res.cursor.firstBatch.reduce((m, spec) => {
            if (spec.hasOwnProperty('buildUUID')) {
                m[spec.spec.name] = spec;
            } else {
                m[spec.name] = spec;
            }
            return m;
        }, {});

        // Check ready indexes.
        for (let name of readyIndexes) {
            assert(indexMap.hasOwnProperty(name),
                   'ready index ' + name + ' missing from listIndexes result: ' + tojson(res));
            const spec = indexMap[name];
            assert(!spec.hasOwnProperty('buildUUID'),
                   'unexpected buildUUID field in ' + name + ' index spec: ' + tojson(spec));
        }

        // Check indexes that are not ready.
        for (let name of notReadyIndexes) {
            assert(indexMap.hasOwnProperty(name),
                   'not-ready index ' + name + ' missing from listIndexes result: ' + tojson(res));

            const spec = indexMap[name];
            if (options.includeBuildUUIDs) {
                assert(spec.hasOwnProperty('spec'),
                       'expected spec field in ' + name + ': ' + tojson(spec));
                assert(spec.hasOwnProperty('buildUUID'),
                       'expected buildUUID field in ' + name + ': ' + tojson(spec));
            } else {
                assert(!spec.hasOwnProperty('buildUUID'),
                       'unexpected buildUUID field in ' + name + ' index spec: ' + tojson(spec));
            }
        }

        return indexMap;
    }

    /**
     * Prevent subsequent index builds from running to completion.
     */
    static pauseIndexBuilds(conn) {
        assert.commandWorked(conn.adminCommand(
            {configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'}));
    }

    /**
     * Unblock current and subsequent index builds.
     */
    static resumeIndexBuilds(conn) {
        assert.commandWorked(
            conn.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'}));
    }
};

const ResumableIndexBuildTest = class {
    /**
     * Returns whether resumable index builds are supported.
     */
    static resumableIndexBuildsEnabled(conn) {
        return assert
            .commandWorked(conn.adminCommand({getParameter: 1, enableResumableIndexBuilds: 1}))
            .enableResumableIndexBuilds;
    }

    /**
     * Returns a version of the given array that has been flattened into one dimension.
     */
    static flatten(array) {
        return array.reduce((accumulator, element) => accumulator.concat(element), []);
    }

    /**
     * Runs createIndexFn in a parellel shell to create indexes, inserting the documents specified
     * by sideWrites into the side writes table.
     *
     * 'createIndexFn' should take three parameters: collection name, index specifications, and
     *   index names.
     *
     * 'indexNames' should follow the exact same format as 'indexSpecs'. For example, if indexSpecs
     *   is [[{a: 1}, {b: 1}], [{c: 1}]], a valid indexNames would look like
     *   [["index_1", "index_2"], ["index_3"]].
     *
     * If {hangBeforeBuildingIndex: true}, returns with the hangBeforeBuildingIndex failpoint
     * enabled and the index builds hanging at this point.
     */
    static createIndexesWithSideWrites(test,
                                       createIndexesFn,
                                       coll,
                                       indexSpecs,
                                       indexNames,
                                       sideWrites,
                                       {hangBeforeBuildingIndex} = {
                                           hangBeforeBuildingIndex: false
                                       }) {
        const primary = test.getPrimary();
        const fp = configureFailPoint(primary, "hangBeforeBuildingIndex");

        const awaitCreateIndexes = new Array(indexSpecs.length);
        for (let i = 0; i < indexSpecs.length; i++) {
            awaitCreateIndexes[i] = startParallelShell(
                funWithArgs(createIndexesFn, coll.getName(), indexSpecs[i], indexNames[i]),
                primary.port);
        }

        // Wait for the index builds to be registered so that they can be recognized using their
        // build UUIDs.
        const indexNamesFlat = ResumableIndexBuildTest.flatten(indexNames);
        let indexes;
        assert.soonNoExcept(function() {
            indexes = IndexBuildTest.assertIndexes(coll,
                                                   indexNamesFlat.length + 1,
                                                   ["_id_"],
                                                   indexNamesFlat,
                                                   {includeBuildUUIDs: true});
            return true;
        });

        // Wait for the index builds to reach the hangBeforeBuildingIndex failpoint.
        for (const indexName of indexNamesFlat) {
            checkLog.containsJson(primary, 4940900, {
                buildUUID: function(uuid) {
                    return uuid["uuid"]["$uuid"] ===
                        extractUUIDFromObject(indexes[indexName].buildUUID);
                }
            });
        }

        assert.commandWorked(coll.insert(sideWrites));

        // Before building the index, wait for the the last op to be committed so that establishing
        // the majority read cursor does not race with step down.
        test.awaitLastOpCommitted();

        if (!hangBeforeBuildingIndex)
            fp.off();

        return awaitCreateIndexes;
    }

    /**
     * The same as createIndexesWithSideWrites() above, specialized for the case of a single index
     * to build.
     */
    static createIndexWithSideWrites(test,
                                     createIndexesFn,
                                     coll,
                                     indexSpec,
                                     indexName,
                                     sideWrites,
                                     {hangBeforeBuildingIndex} = {hangBeforeBuildingIndex: false}) {
        return ResumableIndexBuildTest.createIndexesWithSideWrites(
            test, createIndexesFn, coll, [indexSpec], [indexName], sideWrites, {
                hangBeforeBuildingIndex: hangBeforeBuildingIndex
            })[0];
    }

    /**
     * Asserts that the specified index builds completed successfully.
     */
    static assertCompleted(conn, coll, buildUUIDs, indexNames) {
        for (const buildUUID of buildUUIDs) {
            checkLog.containsJson(conn, 20663, {
                buildUUID: function(uuid) {
                    return uuid["uuid"]["$uuid"] === buildUUID;
                },
                namespace: coll.getFullName()
            });
        }

        IndexBuildTest.assertIndexes(coll, indexNames.length + 1, indexNames.concat("_id_"));
    }

    /**
     * Restarts the given node, ensuring that the index builds write their state to disk upon
     * shutdown and are completed upon startup. Returns the build UUIDs of the index builds that
     * were resumed.
     */
    static restart(rst,
                   conn,
                   coll,
                   indexNames,
                   failPoints,
                   failPointsIteration,
                   shouldComplete = true,
                   failPointAfterStartup) {
        clearRawMongoProgramOutput();

        const indexNamesFlat = ResumableIndexBuildTest.flatten(indexNames);
        const indexNamesFlatSinglePerBuild = new Array(indexNames.length);
        const buildUUIDs = new Array(indexNames.length);
        for (let i = 0; i < indexNames.length; i++) {
            indexNamesFlatSinglePerBuild[i] = indexNames[i][0];
            buildUUIDs[i] = extractUUIDFromObject(
                IndexBuildTest
                    .assertIndexes(coll, indexNamesFlat.length + 1, ["_id_"], indexNamesFlat, {
                        includeBuildUUIDs: true
                    })[indexNames[i][0]]
                    .buildUUID);
        }

        // If there is only one failpoint, its data must include all build UUIDs and index names.
        // Otherwise, each failpoint should only have its respecctive build UUID and index name(s).
        for (let i = 0; i < failPoints.length; i++) {
            failPoints[i].data = {
                buildUUIDs: failPoints.length === 1 ? buildUUIDs : [buildUUIDs[i]],
                indexNames: failPoints.length === 1 ? indexNamesFlatSinglePerBuild
                                                    : [indexNames[i][0]],
                iteration: failPointsIteration
            };
        }

        // Don't interrupt the index builds for shutdown until they are at the desired point.
        const shutdownFpTimesEntered = configureFailPoint(conn, "hangBeforeShutdown").timesEntered;

        const awaitContinueShutdown = startParallelShell(
            funWithArgs(function(failPoints, shutdownFpTimesEntered) {
                load("jstests/libs/fail_point_util.js");

                // Wait until we hang before shutdown to ensure that we do not move the index builds
                // forward before the step down process is complete.
                assert.commandWorked(db.adminCommand({
                    waitForFailPoint: "hangBeforeShutdown",
                    timesEntered: shutdownFpTimesEntered + 1,
                    maxTimeMS: kDefaultWaitForFailPointTimeout
                }));

                // Move the index builds forward to the points that we want them to be interrupted
                // for shutdown at.
                let fp;
                for (const failPoint of failPoints) {
                    fp = configureFailPoint(db.getMongo(), failPoint.name, failPoint.data);
                }
                assert.commandWorked(
                    db.adminCommand({configureFailPoint: "hangBeforeBuildingIndex", mode: "off"}));

                // Wait for the index builds to reach their respective failpoints.
                for (const failPoint of failPoints) {
                    if (failPoint.logIdWithBuildUUID) {
                        for (const buildUUID of failPoint.data.buildUUIDs) {
                            checkLog.containsJson(db.getMongo(), failPoint.logIdWithBuildUUID, {
                                buildUUID: function(uuid) {
                                    return uuid["uuid"]["$uuid"] === buildUUID;
                                }
                            });
                        }
                    } else if (failPoint.logIdWithIndexName) {
                        for (const indexName of failPoint.data.indexNames) {
                            checkLog.containsJson(
                                db.getMongo(), failPoint.logIdWithIndexName, {index: indexName});
                        }
                    } else if (failPoint.useWaitForFailPointForSingleIndexBuild) {
                        assert.eq(
                            failPoints.length,
                            1,
                            "Can only use useWaitForFailPointForSingleIndexBuild with a single index build");
                        fp.wait();
                    } else {
                        assert(
                            false,
                            "Must specify one of logIdWithBuildUUID, logIdWithIndexName, and useWaitForFailPointForSingleIndexBuild");
                    }
                }

                // Disabling this failpoint will allow shutdown to continue and cause the operation
                // context to be killed. This will cause the failpoint specified by failPointName
                // to be interrupted and allow the index builds to be interrupted for shutdown at
                // their current locations.
                assert.commandWorked(
                    db.adminCommand({configureFailPoint: "hangBeforeShutdown", mode: "off"}));
            }, failPoints, shutdownFpTimesEntered), conn.port);

        rst.stop(conn);
        awaitContinueShutdown();

        // Ensure that the resumable index build state was written to disk upon clean shutdown.
        for (const buildUUID of buildUUIDs) {
            assert(RegExp("4841502.*" + buildUUID).test(rawMongoProgramOutput()));
        }

        const setParameter = {logComponentVerbosity: tojson({index: 1})};
        if (failPointAfterStartup) {
            Object.extend(setParameter, {
                ["failpoint." + failPointAfterStartup]: tojson({mode: "alwaysOn"}),
            });
        }

        rst.start(conn, {noCleanData: true, setParameter: setParameter});

        if (shouldComplete) {
            // Ensure that the index builds were completed upon the node starting back up.
            ResumableIndexBuildTest.assertCompleted(conn, coll, buildUUIDs, indexNamesFlat);
        }

        return buildUUIDs;
    }

    /**
     * Makes sure that inserting into a collection outside of an index build works properly,
     * validates indexes on all nodes in the replica set, and drops the index at the end.
     */
    static checkIndexes(rst, dbName, collName, indexNames, postIndexBuildInserts) {
        const primary = rst.getPrimary();
        const coll = primary.getDB(dbName).getCollection(collName);

        rst.awaitReplication();

        assert.commandWorked(coll.insert(postIndexBuildInserts));

        for (const node of rst.nodes) {
            const res = node.getDB(dbName).getCollection(collName).validate();
            assert(res.valid, "Index validation failed: " + tojson(res));
        }

        for (const names of indexNames) {
            assert.commandWorked(coll.dropIndexes(names));
        }
    }

    static checkResume(conn, buildUUIDs, expectedResumePhases, resumeChecks) {
        for (let i = 0; i < buildUUIDs.length; i++) {
            // Ensure that the resume info contains the correct phase to resume from.
            checkLog.containsJson(conn, 4841700, {
                buildUUID: function(uuid) {
                    return uuid["uuid"]["$uuid"] === buildUUIDs[i];
                },
                phase: expectedResumePhases[expectedResumePhases.length === 1 ? 0 : i]
            });

            const resumeCheck = resumeChecks[resumeChecks.length === 1 ? 0 : i];

            if (resumeCheck.numScannedAferResume) {
                // Ensure that the resumed index build resumed the collection scan from the correct
                // location.
                checkLog.containsJson(conn, 20391, {
                    buildUUID: function(uuid) {
                        return uuid["uuid"]["$uuid"] === buildUUIDs[i];
                    },
                    totalRecords: resumeCheck.numScannedAferResume
                });
            } else if (resumeCheck.skippedPhaseLogID) {
                // Ensure that the resumed index build does not perform a phase that it already
                // completed before being interrupted for shutdown.
                assert(!checkLog.checkContainsOnceJson(conn, resumeCheck.skippedPhaseLogID, {
                    buildUUID: function(uuid) {
                        return uuid["uuid"]["$uuid"] === buildUUIDs[i];
                    }
                }),
                       "Found log " + resumeCheck.skippedPhaseLogID + " for index build " +
                           buildUUIDs[i] + " when this phase should not have run after resuming");
            } else {
                assert(false, "Must specify one of numScannedAferResume and skippedPhaseLogID");
            }
        }
    }

    /**
     * Runs a resumable index build test on the provided replica set and namespace.
     *
     * 'indexSpecs' is a 2d array that specifies all indexes that should be built. The first
     *   dimension indicates separate calls to the createIndexes command, while the second
     *   dimension is for indexes that are built together using one call to createIndexes.
     *
     * 'failPoints' is an array of objects that contain two fields: 'name', which specifies the
     *   name of the failpoint, and exactly one of 'logIdWithBuildUUID' and 'logIdWithIndexName'.
     *   The former is used for failpoints whose log message includes the build UUID, and the
     *   latter is used for failpoints whose log message includes the index name. The array must
     *   either contain one element, in which case that one element will be applied to all index
     *   builds specified above, or it must be exactly the length of 'indexSpecs'.
     *
     * 'failPointsIteration' is an integer value used as the 'iteration' field in the failpoint
     *   data for every failpoint specified above.
     *
     * 'expectedResumePhases' is an array of strings that specify the name of the phases that each
     *   index build is expected to resume in. The array must either contain one string, in which
     *   case all index builds will be expected to resume from that phase, or it must be exactly
     *   the length of 'indexSpecs'.
     *
     * 'resumeChecks' is an array of objects that contain exactly one of 'numScannedAferResume' and
     *   'skippedPhaseLogID'. The former is used to verify that the index build scanned the
     *   expected number of documents in the collection scan after resuming. The latter is used for
     *   phases which do not perform a collection scan after resuming, to verify that the index
     *   index build did not resume from an earlier phase than expected. The log message must
     *   contain the buildUUID attribute.
     *
     * 'sideWries' is an array of documents inserted during the initialization phase so that they
     *   are inserted into the side writes table and processed during the drain writes phase.
     *
     * 'postIndexBuildInserts' is an array of documents inserted after the index builds have
     *   completed.
     */
    static run(rst,
               dbName,
               collName,
               indexSpecs,
               failPoints,
               failPointsIteration,
               expectedResumePhases,
               resumeChecks,
               sideWrites = [],
               postIndexBuildInserts = []) {
        const primary = rst.getPrimary();

        if (!ResumableIndexBuildTest.resumableIndexBuildsEnabled(primary)) {
            jsTestLog("Skipping test because resumable index builds are not enabled");
            return;
        }

        const coll = primary.getDB(dbName).getCollection(collName);

        const indexNames = new Array(indexSpecs.length);
        for (let i = 0; i < indexSpecs.length; i++) {
            indexNames[i] = new Array(indexSpecs[i].length);
            for (let j = 0; j < indexSpecs[i].length; j++) {
                indexNames[i][j] = "resumable_index_build_" + i + "_" + j;
            }
        }

        const awaitCreateIndexes = ResumableIndexBuildTest.createIndexesWithSideWrites(
            rst, function(collName, indexSpecs, indexNames) {
                const indexes = new Array(indexSpecs.length);
                for (let i = 0; i < indexSpecs.length; i++) {
                    indexes[i] = {key: indexSpecs[i], name: indexNames[i]};
                }

                assert.commandFailedWithCode(
                    db.runCommand({createIndexes: collName, indexes: indexes}),
                    ErrorCodes.InterruptedDueToReplStateChange);
            }, coll, indexSpecs, indexNames, sideWrites, {hangBeforeBuildingIndex: true});

        const buildUUIDs = ResumableIndexBuildTest.restart(
            rst, primary, coll, indexNames, failPoints, failPointsIteration);

        for (const awaitCreateIndex of awaitCreateIndexes) {
            awaitCreateIndex();
        }

        ResumableIndexBuildTest.checkResume(
            primary, buildUUIDs, expectedResumePhases, resumeChecks);

        ResumableIndexBuildTest.checkIndexes(
            rst, dbName, collName, indexNames, postIndexBuildInserts);
    }

    /**
     * Runs the resumable index build test specified by the provided failpoint information and
     * index spec on the provided replica set and namespace. This will specifically be used to test
     * that resuming an index build on the former primary works. Documents specified by sideWrites
     * will be inserted during the initialization phase so that they are inserted into the side
     * writes table and processed during the drain writes phase.
     */
    static runOnPrimaryToTestCommitQuorum(rst,
                                          dbName,
                                          collName,
                                          indexSpec,
                                          resumeNodeFailPointName,
                                          otherNodeFailPointName,
                                          sideWrites = [],
                                          postIndexBuildInserts = []) {
        const resumeNode = rst.getPrimary();
        const resumeDB = resumeNode.getDB(dbName);

        if (!ResumableIndexBuildTest.resumableIndexBuildsEnabled(resumeNode)) {
            jsTestLog("Skipping test because resumable index builds are not enabled");
            return;
        }

        const secondary = rst.getSecondary();
        const coll = resumeDB.getCollection(collName);
        const indexName = "resumable_index_build";

        const resumeNodeFp = configureFailPoint(resumeNode, resumeNodeFailPointName);
        const otherNodeFp = configureFailPoint(secondary, otherNodeFailPointName);

        const awaitCreateIndex = ResumableIndexBuildTest.createIndexWithSideWrites(
            rst, function(collName, indexSpec, indexName) {
                assert.commandFailedWithCode(
                    db.getCollection(collName).createIndex(indexSpec, {name: indexName}),
                    ErrorCodes.InterruptedDueToReplStateChange);
            }, coll, indexSpec, indexName, sideWrites);

        otherNodeFp.wait();
        resumeNodeFp.wait();

        const buildUUID = extractUUIDFromObject(
            IndexBuildTest
                .assertIndexes(coll, 2, ["_id_"], [indexName], {includeBuildUUIDs: true})[indexName]
                .buildUUID);

        clearRawMongoProgramOutput();
        rst.stop(resumeNode);
        assert(RegExp("4841502.*" + buildUUID).test(rawMongoProgramOutput()));

        rst.start(resumeNode, {noCleanData: true});
        otherNodeFp.off();

        // Ensure that the index build was completed upon the node starting back up.
        ResumableIndexBuildTest.assertCompleted(resumeNode, coll, [buildUUID], [indexName]);

        awaitCreateIndex();

        ResumableIndexBuildTest.checkIndexes(
            rst, dbName, collName, [indexName], postIndexBuildInserts);
    }

    /**
     * Runs the resumable index build test specified by the provided failpoint information and
     * index spec on the provided replica set and namespace. This will specifically be used to test
     * that resuming an index build on a secondary works. Documents specified by sideWrites will be
     * inserted during the initialization phase so that they are inserted into the side writes
     * table and processed during the drain writes phase.
     */
    static runOnSecondary(rst,
                          dbName,
                          collName,
                          indexSpec,
                          resumeNodeFailPointName,
                          resumeNodeFailPointIteration,
                          primaryFailPointName,
                          sideWrites = [],
                          postIndexBuildInserts = []) {
        const primary = rst.getPrimary();
        const coll = primary.getDB(dbName).getCollection(collName);
        const indexName = "resumable_index_build";
        const resumeNode = rst.getSecondary();
        const resumeNodeColl = resumeNode.getDB(dbName).getCollection(collName);

        const resumeNodeFp = configureFailPoint(resumeNode, "hangBeforeBuildingIndex");

        let primaryFp;
        if (primaryFailPointName) {
            primaryFp = configureFailPoint(primary, primaryFailPointName);
        }

        const awaitCreateIndex = ResumableIndexBuildTest.createIndexWithSideWrites(
            rst, function(collName, indexSpec, indexName) {
                // If the secondary is shutdown for too long, the primary will step down until it
                // can reach the secondary again. In this case, the index build will continue in the
                // background.
                assert.commandWorkedOrFailedWithCode(
                    db.getCollection(collName).createIndex(indexSpec, {name: indexName}),
                    ErrorCodes.InterruptedDueToReplStateChange);
            }, coll, indexSpec, indexName, sideWrites);

        resumeNodeFp.wait();

        // We should only check that the index build completes after a restart if the index build is
        // not paused on the primary.
        const buildUUID = ResumableIndexBuildTest.restart(
            rst,
            resumeNode,
            resumeNodeColl,
            [[indexName]],
            [{name: resumeNodeFailPointName, useWaitForFailPointForSingleIndexBuild: true}],
            resumeNodeFailPointIteration,
            !primaryFp /* shouldComplete */)[0];

        if (primaryFp) {
            primaryFp.off();

            // Ensure that the index build was completed after unpausing the primary.
            ResumableIndexBuildTest.assertCompleted(
                resumeNode, resumeNodeColl, [buildUUID], [indexName]);
        }

        awaitCreateIndex();
        ResumableIndexBuildTest.checkIndexes(
            rst, dbName, collName, [indexName], postIndexBuildInserts);
    }

    /**
     * Asserts that the temporary directory for the persisted Sorter data is empty.
     */
    static checkTempDirectoryCleared(primary) {
        const tempDir = primary.dbpath + "/_tmp";

        // If the index build was interrupted for shutdown before anything was inserted into
        // the Sorter, the temp directory may not exist.
        if (!fileExists(tempDir))
            return;

        // Ensure that the persisted Sorter data was cleaned up after failing to resume.
        const files = listFiles(tempDir);
        assert.eq(files.length, 0, files);
    }

    /**
     * Runs the resumable index build test specified by the provided index spec on the provided
     * replica set and namespace. This will be used to test that failing to resume an index build
     * during the setup phase will cause the index build to restart from the beginning instead.
     * The provided failpoint will be set on the node on restart to specify how resuming the index
     * build should fail. Documents specified by sideWrites will be inserted during the
     * initialization phase so that they are inserted into the side writes table and processed
     * during the drain writes phase.
     */
    static runFailToResume(rst,
                           dbName,
                           collName,
                           indexSpec,
                           failpointAfterStartup,
                           sideWrites,
                           postIndexBuildInserts,
                           failWhileParsing = false) {
        const primary = rst.getPrimary();
        const coll = primary.getDB(dbName).getCollection(collName);
        const indexName = "resumable_index_build";

        // Create and drop an index so that the Sorter file name used by the index build
        // interrupted for shutdown is not the same as the Sorter file name used when the index
        // build is restarted.
        assert.commandWorked(coll.createIndex({unused: 1}));
        assert.commandWorked(coll.dropIndex({unused: 1}));

        const awaitCreateIndex = ResumableIndexBuildTest.createIndexWithSideWrites(
            rst, function(collName, indexSpec, indexName) {
                assert.commandFailedWithCode(
                    db.getCollection(collName).createIndex(indexSpec, {name: indexName}),
                    ErrorCodes.InterruptedDueToReplStateChange);
            }, coll, indexSpec, indexName, sideWrites, {hangBeforeBuildingIndex: true});

        const buildUUID = ResumableIndexBuildTest.restart(
            rst,
            primary,
            coll,
            [[indexName]],
            [{name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400}],
            0,
            false /* shouldComplete */,
            failpointAfterStartup)[0];

        awaitCreateIndex();

        // Ensure that the resumable index build failed as expected.
        if (failWhileParsing) {
            assert(RegExp("4916300.*").test(rawMongoProgramOutput()));
            assert(RegExp("22257.*").test(rawMongoProgramOutput()));
        } else {
            assert(RegExp("4841701.*" + buildUUID).test(rawMongoProgramOutput()));
        }

        // Ensure that the index build was completed after it was restarted.
        ResumableIndexBuildTest.assertCompleted(primary, coll, [buildUUID], [indexName]);

        ResumableIndexBuildTest.checkIndexes(
            rst, dbName, collName, [indexName], postIndexBuildInserts);

        const checkLogIdAfterRestart = function(primary, id) {
            rst.stop(primary);
            rst.start(primary, {noCleanData: true});
            checkLog.containsJson(primary, id);
        };

        if (failWhileParsing) {
            // If we fail while parsing, the persisted Sorter data will only be cleaned up after
            // another restart.
            checkLogIdAfterRestart(primary, 5071100);
            ResumableIndexBuildTest.checkTempDirectoryCleared(primary);
        } else {
            ResumableIndexBuildTest.checkTempDirectoryCleared(primary);

            // If we fail after parsing, any remaining internal idents will only be cleaned up
            // after another restart.
            checkLogIdAfterRestart(primary, 22257);
        }
    }

    /**
     * Runs the resumable index build test specified by the provided index spec on the provided
     * replica set and namespace. This will be used to test that shutting down the server before
     * completing an index build resumed during the setup phase will cause the index build to
     * restart from the beginning on a subsequent startup.
     * Two failpoints will be provided:
     *     - the first will be used to pause the index build after the createIndexes command.
     *     - the second will be used to pause the index build after resuming at startup.
     * Documents specified by sideWrites will be inserted during the initialization phase so that
     * they are inserted into the side writes table and processed during the drain writes phase.
     */
    static runResumeInterruptedByShutdown(rst,
                                          dbName,
                                          collName,
                                          indexSpec,
                                          indexName,
                                          failpointBeforeShutdown,
                                          expectedResumePhase,
                                          initialDoc,
                                          sideWrites,
                                          postIndexBuildInserts) {
        const primary = rst.getPrimary();
        const coll = primary.getDB(dbName).getCollection(collName);

        assert.commandWorked(coll.insert(initialDoc));

        const awaitCreateIndex = ResumableIndexBuildTest.createIndexWithSideWrites(
            rst, function(collName, indexSpec, indexName) {
                assert.commandFailedWithCode(
                    db.getCollection(collName).createIndex(indexSpec, {name: indexName}),
                    ErrorCodes.InterruptedDueToReplStateChange);
            }, coll, indexSpec, indexName, sideWrites, {hangBeforeBuildingIndex: true});

        const failpointAfterStartup = {name: "hangAfterIndexBuildFirstDrain", logId: 20666};

        const buildUUID = ResumableIndexBuildTest.restart(rst,
                                                          primary,
                                                          coll,
                                                          [[indexName]],
                                                          [failpointBeforeShutdown],
                                                          0,
                                                          false /* shouldComplete */,
                                                          failpointAfterStartup.name)[0];

        awaitCreateIndex();

        // Ensure that the resume info contains the correct phase to resume from.
        const equalsBuildUUID = function(uuid) {
            return uuid["uuid"]["$uuid"] === buildUUID;
        };
        checkLog.containsJson(primary, 4841700, {
            buildUUID: equalsBuildUUID,
            phase: expectedResumePhase,
        });

        // Ensure that the resumed index build is paused at 'failpointAfterStartup'.
        checkLog.containsJson(primary, failpointAfterStartup.logId, {buildUUID: equalsBuildUUID});

        // After resuming the index build once, it should no longer be resumable if the server shuts
        // down before index build completes. Therefore, we should not see any Sorter data in the
        // temp directory.
        rst.stop(primary);
        ResumableIndexBuildTest.checkTempDirectoryCleared(primary);

        // Interrupting the resumed index build should make it restart from the beginning on next
        // server startup.
        rst.start(primary, {noCleanData: true});
        checkLog.containsJson(primary, 20660, {buildUUID: equalsBuildUUID});

        // Ensure that the index build was completed after it was restarted.
        ResumableIndexBuildTest.assertCompleted(primary, coll, [buildUUID], [indexName]);

        ResumableIndexBuildTest.checkIndexes(
            rst, dbName, collName, [indexName], postIndexBuildInserts);
    }
};
