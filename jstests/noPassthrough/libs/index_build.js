// Helper functions for testing index builds.

class IndexBuildTest {
    /**
     * Starts an index build in a separate mongo shell process with given options.
     */
    static startIndexBuild(conn, ns, keyPattern, options) {
        options = options || {};
        return startParallelShell('const coll = db.getMongo().getCollection("' + ns + '");' +
                                      'assert.commandWorked(coll.createIndex(' +
                                      tojson(keyPattern) + ', ' + tojson(options) + '));',
                                  conn.port);
    }

    /**
     * Returns the op id for the running index build on the provided 'collectionName' and
     * 'indexName', or any index build if either is undefined. Returns -1 if there is no current
     * index build.
     * Accepts optional filter that can be used to customize the db.currentOp() query.
     */
    static getIndexBuildOpId(database, collectionName, indexName, filter) {
        const result = database.currentOp(filter || true);
        assert.commandWorked(result);
        let indexBuildOpId = -1;
        let indexBuildObj = {};
        let indexBuildNamespace = "";

        result.inprog.forEach(function(op) {
            if (op.op != 'command') {
                return;
            }
            if (op.command.createIndexes === undefined) {
                return;
            }
            // If no collection is provided, return any index build.
            if (!collectionName || op.command.createIndexes === collectionName) {
                op.command.indexes.forEach((index) => {
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
        const inprog = database.currentOp({opid: opId}).inprog;
        assert.eq(1,
                  inprog.length,
                  'unable to find opid ' + opId + ' in currentOp() result: ' +
                      tojson(database.currentOp()));
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
     */
    static assertIndexes(coll, numIndexes, readyIndexes, notReadyIndexes, options) {
        notReadyIndexes = notReadyIndexes || [];
        options = options || {};

        let res = coll.runCommand("listIndexes", options);
        assert.eq(numIndexes,
                  res.cursor.firstBatch.length,
                  'unexpected number of indexes in collection: ' + tojson(res));

        // First batch contains all the indexes in the collection.
        assert.eq(0, res.cursor.id);

        // A map of index specs keyed by index name.
        const indexMap = res.cursor.firstBatch.reduce(
            (m, spec) => {
                if (spec.hasOwnProperty('buildUUID')) {
                    m[spec.spec.name] = spec;
                } else {
                    m[spec.name] = spec;
                }
                return m;
            },
            {});

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
}
