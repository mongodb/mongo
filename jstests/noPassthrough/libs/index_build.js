// Helper functions for testing index builds.

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/uuid_util.js");

var IndexBuildTest = class {
    /**
     * Starts an index build in a separate mongo shell process with given options.
     * Ensures the index build worked or failed with one of the expected failures.
     */
    static startIndexBuild(conn, ns, keyPattern, options, expectedFailures) {
        options = options || {};
        expectedFailures = expectedFailures || [];

        if (Array.isArray(keyPattern)) {
            return startParallelShell(
                'const coll = db.getMongo().getCollection("' + ns + '");' +
                    'assert.commandWorkedOrFailedWithCode(coll.createIndexes(' +
                    JSON.stringify(keyPattern) + ', ' + tojson(options) + '), ' +
                    JSON.stringify(expectedFailures) + ');',
                conn.port);
        } else {
            return startParallelShell('const coll = db.getMongo().getCollection("' + ns + '");' +
                                          'assert.commandWorkedOrFailedWithCode(coll.createIndex(' +
                                          tojson(keyPattern) + ', ' + tojson(options) + '), ' +
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

    /**
     * Returns true if majority commit quorum is supported by two phase index builds.
     */
    static indexBuildCommitQuorumEnabled(conn) {
        return assert
            .commandWorked(conn.adminCommand({getParameter: 1, enableIndexBuildCommitQuorum: 1}))
            .enableIndexBuildCommitQuorum;
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
     * Creates the specified index in a parallel shell and expects it to fail with
     * InterruptedDueToReplStateChange. After calling this function and before waiting on the
     * returned parallel shell to complete, the test should cause the node to go through a replica
     * set state transition.
     */
    static createIndex(primary, collName, indexSpec, indexName) {
        return startParallelShell(
            funWithArgs(function(collName, indexSpec, indexName) {
                assert.commandFailedWithCode(
                    db.getCollection(collName).createIndex(indexSpec, {name: indexName}),
                    ErrorCodes.InterruptedDueToReplStateChange);
            }, collName, indexSpec, indexName), primary.port);
    }

    /**
     * Waits for the specified index build to be interrupted and then disables the given failpoint.
     */
    static disableFailPointAfterInterruption(conn, failPointName, buildUUID) {
        return startParallelShell(
            funWithArgs(function(failPointName, buildUUID) {
                // Wait for the index build to be interrupted.
                checkLog.containsJson(db.getMongo(), 20449, {
                    buildUUID: function(uuid) {
                        return uuid["uuid"]["$uuid"] === buildUUID;
                    },
                    error: function(error) {
                        return error.code === ErrorCodes.InterruptedDueToReplStateChange;
                    }
                });

                // Once the index build has been interrupted, disable the failpoint so that shutdown
                // or stepdown can proceed.
                assert.commandWorked(
                    db.adminCommand({configureFailPoint: failPointName, mode: "off"}));
            }, failPointName, buildUUID), conn.port);
    }

    /**
     * Inserts the given documents once an index build reaches the end of the bulk load phase so
     * that the documents are inserted into the side writes table for that index build.
     */
    static insertIntoSideWritesTable(primary, collName, docs) {
        return startParallelShell(funWithArgs(function(collName, docs) {
                                      if (!docs)
                                          return;

                                      load("jstests/libs/fail_point_util.js");

                                      const sideWritesFp = configureFailPoint(
                                          db.getMongo(), "hangAfterSettingUpIndexBuild");
                                      sideWritesFp.wait();

                                      assert.commandWorked(db.getCollection(collName).insert(docs));

                                      sideWritesFp.off();
                                  }, collName, docs), primary.port);
    }

    /**
     * Asserts that the specific index build successfully completed.
     */
    static assertCompleted(conn, coll, buildUUID, numIndexes, indexes) {
        checkLog.containsJson(conn, 20663, {
            buildUUID: function(uuid) {
                return uuid["uuid"]["$uuid"] === buildUUID;
            },
            namespace: coll.getFullName()
        });
        IndexBuildTest.assertIndexes(coll, numIndexes, indexes);
    }

    /**
     * Restarts the given node, ensuring that the the index build with name indexName has its state
     * written to disk upon shutdown and is completed upon startup.
     */
    static restart(rst, conn, coll, indexName, failPointName) {
        clearRawMongoProgramOutput();

        const buildUUID = extractUUIDFromObject(
            IndexBuildTest
                .assertIndexes(coll, 2, ["_id_"], [indexName], {includeBuildUUIDs: true})[indexName]
                .buildUUID);

        const awaitDisableFailPoint = ResumableIndexBuildTest.disableFailPointAfterInterruption(
            conn, failPointName, buildUUID);

        rst.stop(conn);
        awaitDisableFailPoint();

        // Ensure that the resumable index build state was written to disk upon clean shutdown.
        assert(RegExp("4841502.*" + buildUUID).test(rawMongoProgramOutput()));

        rst.start(conn, {noCleanData: true});

        // Ensure that the index build was completed upon the node starting back up.
        ResumableIndexBuildTest.assertCompleted(conn, coll, buildUUID, 2, ["_id_", indexName]);
    }

    /**
     * Runs the resumable index build test specified by the provided failpoint information and
     * index spec on the provided replica set and namespace. Document(s) specified by
     * insertIntoSideWritesTable will be inserted after the bulk load phase so that they are
     * inserted into the side writes table and processed during the drain writes phase.
     */
    static run(rst,
               dbName,
               collName,
               indexSpec,
               failPointName,
               failPointData,
               insertIntoSideWritesTable,
               postIndexBuildInserts = {}) {
        const primary = rst.getPrimary();
        const coll = primary.getDB(dbName).getCollection(collName);
        const indexName = "resumable_index_build";

        const fp = configureFailPoint(primary, failPointName, failPointData);

        const awaitInsertIntoSideWritesTable = ResumableIndexBuildTest.insertIntoSideWritesTable(
            primary, collName, insertIntoSideWritesTable);

        const awaitCreateIndex =
            ResumableIndexBuildTest.createIndex(primary, coll.getName(), indexSpec, indexName);

        fp.wait();

        ResumableIndexBuildTest.restart(rst, primary, coll, indexName, failPointName);

        awaitInsertIntoSideWritesTable();
        awaitCreateIndex();

        if (postIndexBuildInserts) {
            assert.commandWorked(coll.insert(postIndexBuildInserts));
        }

        assert(coll.validate(), "Index validation failed");

        assert.commandWorked(coll.dropIndex(indexName));
    }
};
