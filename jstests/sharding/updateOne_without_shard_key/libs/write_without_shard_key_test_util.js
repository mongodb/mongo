/*
 * Utilities for performing writes without shard key under various test configurations.
 */

'use strict';
var WriteWithoutShardKeyTestUtil = (function() {
    const Configurations = {
        noSession: "Running without a session",
        sessionNotRetryableWrite:
            "Running within a session but not as a retryable write or transaction",
        sessionRetryableWrite: "Running as a retryable write",
        transaction: "Running as a transaction"
    };

    const OperationType = {
        updateOne: 1,
        deleteOne: 2,
        findAndModifyUpdate: 3,
        findAndModifyRemove: 4,
    };

    function setupShardedCollection(st, nss, shardKey, splitPoints, chunksToMove) {
        const splitString = nss.split(".");
        const dbName = splitString[0];

        assert.commandWorked(st.s.adminCommand({enablesharding: dbName}));
        st.ensurePrimaryShard(dbName, st.shard0.shardName);
        assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: shardKey}));

        for (let splitPoint of splitPoints) {
            assert.commandWorked(st.s.adminCommand({split: nss, middle: splitPoint}));
        }

        for (let {query, shard} of chunksToMove) {
            assert.commandWorked(st.s.adminCommand({
                moveChunk: nss,
                find: query,
                to: shard,
            }));
        }
    }

    /*
     * Validates the result of doing a update without a shard key.
     * - For non-replacement style updates,
     * we expect that for all of the documents in the collection, a modification is only applied to
     * one of the matching documents.
     *
     * - For replacement style updates, we expect that the final replacement modification is a
     * unique document in the collection.
     */
    function validateResultUpdate(docs, expectedMods, isReplacementTest) {
        expectedMods.forEach(mod => {
            if (isReplacementTest) {
                let matches = 0;
                docs.forEach(doc => {
                    let {_id: idDoc, ...docWithoutId} = doc;
                    if (tojson(docWithoutId) == tojson(mod)) {
                        matches++;
                    }
                });
                assert.eq(matches, 1);
            } else {
                let field = Object.keys(mod)[0];
                let value = mod[field];
                let docsHaveFieldMatchArray = [];
                docs.forEach(doc => {
                    if (doc[field] == value) {
                        docsHaveFieldMatchArray.push(doc[field] == value);
                    }
                });

                assert.eq(docsHaveFieldMatchArray.length, 1);
            }
        });
    }

    /*
     * Validates that we've successfully removed the appropriate number of documents.
     */
    function validateResultDelete(numDocsLeft, expectedDocsLeft) {
        assert.eq(numDocsLeft, expectedDocsLeft);
    }

    /*
     * Inserts a batch of documents and runs a write without shard key and returns all of the
     * documents inserted.
     */
    function insertDocsAndRunCommand(conn,
                                     collName,
                                     docsToInsert,
                                     cmdObj,
                                     operationType,
                                     expectedResponse,
                                     expectedRetryResponse) {
        assert.commandWorked(conn.getCollection(collName).insert(docsToInsert));
        let res = assert.commandWorked(conn.runCommand(cmdObj));
        if (operationType === OperationType.updateOne) {
            assert.eq(expectedResponse.n, res.n);
            assert.eq(expectedResponse.nModified, res.nModified);
        } else if (operationType === OperationType.deleteOne) {
            assert.eq(expectedResponse.n, res.n);
        } else {
            assert.eq(expectedResponse.lastErrorObject.n, res.lastErrorObject.n);
            assert.eq(expectedResponse.lastErrorObject.updateExisting,
                      res.lastErrorObject.updateExisting);
            assert((typeof res.value) !== "undefined");

            // For findAndModify, get the pre/post image document to compare for retryability tests
            Object.assign(expectedRetryResponse, {value: res.value});
        }
        return conn.getCollection(collName).find({}).toArray();
    }

    /*
     * Retry a retryable write and expect to get the retried response back.
     */
    function retryableWriteTest(conn, cmdObj, operationType, expectedRetryResponse) {
        let res = assert.commandWorked(conn.runCommand(cmdObj));
        if (operationType === OperationType.updateOne) {
            assert.eq(expectedRetryResponse.n, res.n);
            assert.eq(expectedRetryResponse.nModified, res.nModified);
            assert.eq(expectedRetryResponse.retriedStmtIds, res.retriedStmtIds);
        } else if (operationType === OperationType.deleteOne) {
            assert.eq(expectedRetryResponse.n, res.n);
            assert.eq(expectedRetryResponse.retriedStmtIds, res.retriedStmtIds);
        } else {
            assert.eq(expectedRetryResponse.lastErrorObject.n, res.lastErrorObject.n);
            assert.eq(expectedRetryResponse.lastErrorObject.updateExisting,
                      res.lastErrorObject.updateExisting);
            assert.eq(expectedRetryResponse.retriedStmtId, res.retriedStmtId);
            assert.eq(expectedRetryResponse.value, res.value);
        }
    }

    /*
     * Runs a test using a cmdObj with multiple configurations e.g. with/without a session etc.
     */
    function runTestWithConfig(conn, testCase, config, operationType) {
        // If a test case does not specify distinct options, we can run the test case as is
        // just once.
        if (!testCase.options) {
            testCase.options = [{}];
        }

        testCase.options.forEach(option => {
            jsTestLog(testCase.logMessage + "\n" +
                      "For option: " + tojson(option) + "\n" + config);
            let newCmdObj = Object.assign({}, testCase.cmdObj, option);

            let allMatchedDocs;
            let dbConn;
            if (config == Configurations.transaction) {
                conn.startTransaction();
                dbConn = conn.getDatabase(testCase.dbName);
                allMatchedDocs = insertDocsAndRunCommand(dbConn,
                                                         testCase.collName,
                                                         testCase.docsToInsert,
                                                         newCmdObj,
                                                         operationType,
                                                         testCase.expectedResponse,
                                                         testCase.expectedRetryResponse);
                conn.commitTransaction_forTesting();
            } else {
                switch (config) {
                    case Configurations.sessionNotRetryableWrite:
                    case Configurations.sessionRetryableWrite:
                        dbConn = conn.getDatabase(testCase.dbName);
                        break;
                    default:
                        dbConn = conn.getDB(testCase.dbName);
                }

                if (config == Configurations.sessionRetryableWrite) {
                    const retryableWriteFields = {
                        lsid: {id: UUID()},
                        txnNumber: NumberLong(0),
                    };
                    Object.assign(newCmdObj, retryableWriteFields);
                }
                allMatchedDocs = insertDocsAndRunCommand(dbConn,
                                                         testCase.collName,
                                                         testCase.docsToInsert,
                                                         newCmdObj,
                                                         operationType,
                                                         testCase.expectedResponse,
                                                         testCase.expectedRetryResponse);
            }

            // Test that the retryable write response is recovered.
            if (testCase.retryableWriteTest) {
                retryableWriteTest(
                    dbConn, newCmdObj, operationType, testCase.expectedRetryResponse);
            }

            switch (operationType) {
                case OperationType.updateOne:
                case OperationType.findAndModifyUpdate:
                    validateResultUpdate(
                        allMatchedDocs, testCase.expectedMods, testCase.replacementDocTest);
                    break;
                case OperationType.deleteOne:
                    validateResultDelete(
                        allMatchedDocs.length,
                        testCase.docsToInsert.length - testCase.expectedResponse.n);
                    break;
                case OperationType.findAndModifyRemove:
                    validateResultDelete(
                        allMatchedDocs.length,
                        testCase.docsToInsert.length - testCase.expectedResponse.lastErrorObject.n);
                    break;
                default:
                    throw 'Invalid OperationType.';
            }

            // Clean up the collection for the next test case without dropping the collection.
            assert.commandWorked(dbConn.getCollection(testCase.collName).remove({}));

            // Check that the retryable write response is still recoverable even if the document was
            // removed.
            if (testCase.retryableWriteTest) {
                retryableWriteTest(
                    dbConn, newCmdObj, operationType, testCase.expectedRetryResponse);
            }

            // Killing the session after the command is done using it to not excessively leave
            // unused sessions around.
            if (config == Configurations.transaction ||
                config == Configurations.sessionNotRetryableWrite ||
                config == Configurations.sessionRetryableWrite) {
                conn.endSession();
            }
        });
    }

    /*
     * Returns a connection with or without a session started based on the provided
     * configuration.
     */
    function getClusterConnection(st, config) {
        switch (config) {
            case Configurations.noSession:
                return st.s;
            case Configurations.sessionNotRetryableWrite:
                return st.s.startSession();
            case Configurations.sessionRetryableWrite:
            case Configurations.transaction:
                return st.s.startSession({retryWrites: true});
            default:
                throw 'Invalid configuration chosen.';
        }
    }

    /*
     * Checks if the write without shard key feature is enabled.
     */
    function isWriteWithoutShardKeyFeatureEnabled(conn) {
        // The feature flag spans 6.2 and current master, while the actual logic only exists
        // on 6.3 and later.
        return (jsTestOptions().mongosBinVersion !== "last-lts" &&
                jsTestOptions().mongosBinVersion !== "last-continuous" &&
                assert
                    .commandWorked(conn.adminCommand(
                        {getParameter: 1, featureFlagUpdateOneWithoutShardKey: 1}))
                    .featureFlagUpdateOneWithoutShardKey.value);
    }

    return {
        setupShardedCollection,
        getClusterConnection,
        runTestWithConfig,
        insertDocsAndRunCommand,
        Configurations,
        OperationType,
        isWriteWithoutShardKeyFeatureEnabled
    };
})();
