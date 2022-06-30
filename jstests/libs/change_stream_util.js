/*
 * A class with helper functions which operate on change streams. The class maintains a list of
 * opened cursors and kills them on cleanup.
 */

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

/**
 * Enumeration of the possible types of change streams.
 */
const ChangeStreamWatchMode = Object.freeze({
    kCollection: 1,
    kDb: 2,
    kCluster: 3,
});

/**
 * Returns a truncated json object if the size of 'jsonObj' is greater than 'maxErrorSizeBytes'.
 */
function tojsonMaybeTruncate(jsonObj) {
    // Maximum size for the json object.
    const maxErrorSizeBytes = 1024 * 1024;

    return tojson(jsonObj).substring(0, maxErrorSizeBytes);
}

/**
 * Returns true if server version is 5.1 or above. Version 5.1 and above optimizes the change stream
 * pipeline.
 *
 * TODO SERVER-60736: remove this function and update all call-sites.
 */
function isChangeStreamsOptimizationEnabled(db) {
    return MongoRunner.compareBinVersions(db.getSiblingDB("admin").serverStatus().version, "5.1") !=
        -1;
}

/**
 * Returns true if feature flag 'featureFlagChangeStreamPreAndPostImages' is enabled, false
 * otherwise.
 */
function isChangeStreamPreAndPostImagesEnabled(db) {
    const getParam = db.adminCommand({getParameter: 1, featureFlagChangeStreamPreAndPostImages: 1});
    return getParam.hasOwnProperty("featureFlagChangeStreamPreAndPostImages") &&
        getParam.featureFlagChangeStreamPreAndPostImages.value;
}

/**
 * Returns true if feature flag 'featureFlagChangeStreamsRewrite' is enabled, false otherwise.
 */
function isChangeStreamsRewriteEnabled(db) {
    const getParam = db.adminCommand({getParameter: 1, featureFlagChangeStreamsRewrite: 1});
    return getParam.hasOwnProperty("featureFlagChangeStreamsRewrite") &&
        getParam.featureFlagChangeStreamsRewrite.value;
}

/**
 * Returns true if feature flag 'featureFlagChangeStreamsVisibility' is enabled, false otherwise.
 */
function isChangeStreamsVisibilityEnabled(db) {
    const getParam = db.adminCommand({getParameter: 1, featureFlagChangeStreamsVisibility: 1});
    return getParam.hasOwnProperty("featureFlagChangeStreamsVisibility") &&
        getParam.featureFlagChangeStreamsVisibility.value;
}

/**
 * Helper function used internally by ChangeStreamTest. If no passthrough is active, it is exactly
 * the same as calling db.runCommand. If a passthrough is active and has defined a function
 * 'changeStreamPassthroughAwareRunCommand', then this method will be overridden to allow individual
 * streams to explicitly exempt themselves from being modified by the passthrough.
 */
function isChangeStreamPassthrough() {
    return typeof changeStreamPassthroughAwareRunCommand != 'undefined';
}

/**
 * Helper function to retrieve the type of change stream based on the passthrough suite in which the
 * test is being run. If no passthrough is active, this method returns the kCollection watch mode.
 */
function changeStreamPassthroughType() {
    return typeof ChangeStreamPassthroughHelpers === 'undefined'
        ? ChangeStreamWatchMode.kCollection
        : ChangeStreamPassthroughHelpers.passthroughType();
}

const runCommandChangeStreamPassthroughAware =
    (!isChangeStreamPassthrough() ? ((db, cmdObj) => db.runCommand(cmdObj))
                                  : changeStreamPassthroughAwareRunCommand);

/**
 * Asserts that the given opType triggers an invalidate entry depending on the type of change
 * stream.
 *     - single collection streams: drop, rename, and dropDatabase.
 *     - whole DB streams: dropDatabase.
 *     - whole cluster streams: none.
 * Returns the invalidate document if there was one, or null otherwise.
 */
function assertInvalidateOp({cursor, opType}) {
    if (!isChangeStreamPassthrough() ||
        (changeStreamPassthroughType() == ChangeStreamWatchMode.kDb && opType == "dropDatabase")) {
        assert.soon(() => cursor.hasNext());
        const invalidate = cursor.next();
        assert.eq(invalidate.operationType, "invalidate");
        assert(!cursor.hasNext());
        assert(cursor.isExhausted());
        assert(cursor.isClosed());
        return invalidate;
    }
    return null;
}

function canonicalizeEventForTesting(event, expected) {
    for (let fieldName of ["_id",
                           "clusterTime",
                           "txnNumber",
                           "lsid",
                           "collectionUUID",
                           "wallTime",
                           "operationDescription"]) {
        if (!expected.hasOwnProperty(fieldName)) {
            delete event[fieldName];
        }
    }

    if (!expected.hasOwnProperty("updateDescription"))
        delete event.updateDescription;

    return event;
}

/**
 * Returns true if a change stream event matches the given expected event, false otherwise. Ignores
 * the resume token, clusterTime, and other unknowable fields unless they are explicitly listed in
 * the expected event.
 */
function isChangeStreamEventEq(actualEvent, expectedEvent) {
    const testEvent = canonicalizeEventForTesting(Object.assign({}, actualEvent), expectedEvent);
    return friendlyEqual(sortDoc(testEvent), sortDoc(expectedEvent));
}

/**
 * Helper to check whether a change event matches the given expected event. Throws assertion failure
 * if change events do not match.
 */
function assertChangeStreamEventEq(actualEvent, expectedEvent) {
    assert(isChangeStreamEventEq(actualEvent, expectedEvent),
           () => "Change events did not match. Expected: " + tojsonMaybeTruncate(expectedEvent) +
               ", Actual: " + tojsonMaybeTruncate(actualEvent));
}

function ChangeStreamTest(_db, name = "ChangeStreamTest") {
    load("jstests/libs/namespace_utils.js");  // For getCollectionNameFromFullNamespace.

    // Keeps track of cursors opened during the test so that we can be sure to
    // clean them up before the test completes.
    let _allCursors = [];
    let self = this;

    // Prevent accidental usages of the default db.
    const db = null;

    /**
     * Starts a change stream cursor with the given pipeline on the given collection. It uses
     * the 'aggregateOptions' if provided and saves the cursor so that it can be cleaned up later.
     * If 'doNotModifyInPassthroughs' is 'true' and the test is running in a $changeStream
     * upconversion passthrough, then this stream will not be modified and will run as though no
     * passthrough were active.
     *
     * Returns the cursor returned by the 'aggregate' command.
     */
    self.startWatchingChanges = function(
        {pipeline, collection, aggregateOptions, doNotModifyInPassthroughs}) {
        aggregateOptions = aggregateOptions || {};
        aggregateOptions.cursor = aggregateOptions.cursor || {batchSize: 1};

        // The 'collection' argument may be a collection name, DBCollection object, or '1' which
        // indicates all collections in _db.
        assert(collection instanceof DBCollection || typeof collection === "string" ||
               collection === 1);
        const collName = (collection instanceof DBCollection ? collection.getName() : collection);

        let res = assert.commandWorked(runCommandChangeStreamPassthroughAware(
            _db,
            Object.merge({aggregate: collName, pipeline: pipeline}, aggregateOptions),
            doNotModifyInPassthroughs));
        assert.neq(res.cursor.id, 0);
        _allCursors.push({db: _db.getName(), coll: collName, cursorId: res.cursor.id});
        return res.cursor;
    };

    /**
     * Returns a change stream cursor that listens for every change in the cluster. Assumes that the
     * ChangeStreamTest has been created on the 'admin' db, and will assert if not. It uses the
     * 'aggregateOptions' if provided and saves the cursor so that it can be cleaned up later.
     */
    self.startWatchingAllChangesForCluster = function(aggregateOptions) {
        assert.eq(_db.getName(), "admin");
        return self.startWatchingChanges({
            pipeline: [{$changeStream: {allChangesForCluster: true}}],
            collection: 1,
            aggregateOptions: aggregateOptions
        });
    };

    /**
     * Issues a 'getMore' on the provided cursor and returns the cursor returned.
     */
    self.getNextBatch = function(cursor) {
        const collName = getCollectionNameFromFullNamespace(cursor.ns);
        return assert
            .commandWorked(_db.runCommand({getMore: cursor.id, collection: collName, batchSize: 1}))
            .cursor;
    };

    /**
     * Returns the next batch of documents from the cursor. This encapsulates logic for checking
     * if it's the first batch or another batch afterwards.
     */
    function getBatchFromCursorDocument(cursor) {
        return (cursor.nextBatch === undefined) ? cursor.firstBatch : cursor.nextBatch;
    }

    /**
     * Returns the next document from a cursor or returns null if there wasn't one.
     * This does not issue any getMores, instead relying off the batch inside 'cursor'.
     */
    function getNextDocFromCursor(cursor) {
        let nextBatch = getBatchFromCursorDocument(cursor);
        if (nextBatch.length === 0) {
            return null;
        }
        assert.eq(
            nextBatch.length, 1, "Batch length wasn't 0 or 1: " + tojsonMaybeTruncate(cursor));
        return nextBatch[0];
    }

    self.getNextChanges = function(cursor, numChanges, skipFirst) {
        let changes = [];

        for (let i = 0; i < numChanges; i++) {
            // Since the first change may be on the original cursor, we need to check for that
            // change on the cursor before we move the cursor forward.
            if (i === 0 && !skipFirst) {
                changes[0] = getNextDocFromCursor(cursor);
                if (changes[0]) {
                    continue;
                }
            }

            assert.soon(
                () => {
                    assert.neq(
                        cursor.id,
                        NumberLong(0),
                        "Cursor has been closed unexpectedly. Observed change stream events: " +
                            tojsonMaybeTruncate(changes));
                    cursor = self.getNextBatch(cursor);
                    changes[i] = getNextDocFromCursor(cursor);
                    return changes[i] !== null;
                },
                () => {
                    return "timed out waiting for another result from the change stream, observed changes: " +
                        tojsonMaybeTruncate(changes) + ", expected changes: " + numChanges;
                });
        }

        return changes;
    };

    /**
     * Checks if the change has been invalidated.
     */
    function isInvalidated(change) {
        return change.operationType === "invalidate";
    }

    /**
     * Asserts that the last observed change was the change we expect to see. This also asserts
     * that if we do not expect the cursor to be invalidated, that we do not see the cursor
     * invalidated. Omits the observed change's resume token and cluster time from the comparison,
     * unless the expected change explicitly lists an '_id' or 'clusterTime' field to compare
     * against.
     */
    function assertChangeIsExpected(
        expectedChanges, numChangesSeen, observedChanges, expectInvalidate) {
        if (expectedChanges) {
            assertChangeStreamEventEq(observedChanges[numChangesSeen],
                                      expectedChanges[numChangesSeen]);
        } else if (!expectInvalidate) {
            assert(!isInvalidated(observedChanges[numChangesSeen]),
                   "Change was invalidated when it should not have been. Number of changes seen: " +
                       numChangesSeen +
                       ", observed changes: " + tojsonMaybeTruncate(observedChanges) +
                       ", expected changes: " + tojsonMaybeTruncate(expectedChanges));
        }
    }

    /**
     * Iterates through the change stream and asserts that the next changes are the expected ones.
     * This can be provided with either an expected size or a list of expected changes.
     * If 'expectInvalidate' is provided, then it will expect the change stream to be invalidated
     * at the end. The caller is still expected to provide an invalidate entry in 'expectedChanges'.
     *
     * Returns a list of the changes seen.
     */
    self.assertNextChangesEqual = function({
        cursor,
        expectedChanges,
        expectedNumChanges,
        expectInvalidate,
        skipFirstBatch,
        ignoreOrder
    }) {
        expectInvalidate = expectInvalidate || false;
        skipFirstBatch = skipFirstBatch || false;

        // Assert that the expected length matches the length of the expected batch.
        if (expectedChanges !== undefined && expectedNumChanges !== undefined) {
            assert.eq(expectedChanges.length,
                      expectedNumChanges,
                      "Expected change's size must match expected number of changes");
        }

        // Convert 'expectedChanges' to an array, even if it contains just a single element.
        if (expectedChanges !== undefined && !(expectedChanges instanceof Array)) {
            let arrayVersion = new Array;
            arrayVersion.push(expectedChanges);
            expectedChanges = arrayVersion;
        }

        // Set the expected number of changes based on the size of the expected change list.
        if (expectedNumChanges === undefined) {
            assert.neq(expectedChanges, undefined);
            expectedNumChanges = expectedChanges.length;
        }

        let changes = self.getNextChanges(cursor, expectedNumChanges, skipFirstBatch);
        if (ignoreOrder) {
            const errMsgFunc = () =>
                `${tojsonMaybeTruncate(changes)} != ${tojsonMaybeTruncate(expectedChanges)}`;
            assert.eq(changes.length, expectedNumChanges, errMsgFunc);
            for (let i = 0; i < changes.length; i++) {
                assert(expectedChanges.some(expectedChange => {
                    return isChangeStreamEventEq(changes[i], expectedChange);
                }),
                       errMsgFunc);
            }
        } else {
            for (let i = 0; i < changes.length; i++) {
                assertChangeIsExpected(expectedChanges, i, changes, expectInvalidate);
            }
        }

        // If we expect invalidation, the final change should have operation type "invalidate".
        if (expectInvalidate) {
            assert(isInvalidated(changes[changes.length - 1]),
                   "Last change was not invalidated when it was expected: " +
                       tojsonMaybeTruncate(changes));

            // We make sure that the next batch kills the cursor after an invalidation entry.
            let finalCursor = self.getNextBatch(cursor);
            assert.eq(finalCursor.id,
                      0,
                      "Final cursor was not killed: " + tojsonMaybeTruncate(finalCursor));
        }

        return changes;
    };

    /**
     * Iterates through the change stream and asserts that the next changes are the expected ones.
     * The order of the change events from the cursor relative to their order in the list of
     * expected changes is ignored, however.
     *
     * Returns a list of the changes seen.
     */
    self.assertNextChangesEqualUnordered = function(
        {cursor, expectedChanges, expectedNumChanges, expectInvalidate, skipFirstBatch}) {
        return self.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: expectedChanges,
            expectedNumChanges: expectedNumChanges,
            expectInvalidate: expectInvalidate,
            skipFirstBatch: skipFirstBatch,
            ignoreOrder: true
        });
    };

    /**
     * Retrieves the next batch in the change stream and confirms that it is empty.
     */
    self.assertNoChange = function(cursor) {
        cursor = self.getNextBatch(cursor);
        assert.eq(
            0, cursor.nextBatch.length, () => "Cursor had changes: " + tojsonMaybeTruncate(cursor));
    };

    /**
     * Gets the next document in the change stream. This always executes a 'getMore' first.
     * If the current batch has a document in it, that one will be ignored.
     */
    self.getOneChange = function(cursor, expectInvalidate = false) {
        changes = self.getNextChanges(cursor, 1, true);

        if (expectInvalidate) {
            assert(isInvalidated(changes[changes.length - 1]),
                   "Last change was not invalidated when it was expected: " +
                       tojsonMaybeTruncate(changes));

            // We make sure that the next batch kills the cursor after an invalidation entry.
            let finalCursor = self.getNextBatch(cursor);
            assert.eq(finalCursor.id,
                      0,
                      "Final cursor was not killed: " + tojsonMaybeTruncate(finalCursor));
        }

        return changes[0];
    };

    /**
     * Kills all outstanding cursors.
     */
    self.cleanUp = function() {
        for (let testCursor of _allCursors) {
            if (typeof testCursor.coll === "string") {
                assert.commandWorked(_db.getSiblingDB(testCursor.db).runCommand({
                    killCursors: testCursor.coll,
                    cursors: [testCursor.cursorId]
                }));
            } else if (testCursor.coll == 1) {
                // Collection '1' indicates that the change stream was opened against an entire
                // database and is considered 'collectionless'.
                assert.commandWorked(_db.getSiblingDB(testCursor.db).runCommand({
                    killCursors: "$cmd.aggregate",
                    cursors: [testCursor.cursorId]
                }));
            }
        }
    };

    /**
     * Returns the document to be used for the value of a $changeStream stage, given a watchMode
     * of type ChangeStreamWatchMode and optional resumeAfter value.
     */
    self.getChangeStreamStage = function(watchMode, resumeAfter) {
        const changeStreamDoc = {};
        if (resumeAfter) {
            changeStreamDoc.resumeAfter = resumeAfter;
        }

        if (watchMode == ChangeStreamWatchMode.kCluster) {
            changeStreamDoc.allChangesForCluster = true;
        }
        return changeStreamDoc;
    };

    /**
     * Create a change stream of the given watch mode (see ChangeStreamWatchMode) on the given
     * collection. Will resume from a given point if resumeAfter is specified.
     */
    self.getChangeStream = function({watchMode, coll, resumeAfter}) {
        return self.startWatchingChanges({
            pipeline: [{$changeStream: self.getChangeStreamStage(watchMode, resumeAfter)}],
            collection: (watchMode == ChangeStreamWatchMode.kCollection ? coll : 1),
            // Use a batch size of 0 to prevent any notifications from being returned in the first
            // batch. These would be ignored by ChangeStreamTest.getOneChange().
            aggregateOptions: {cursor: {batchSize: 0}},
        });
    };

    /**
     * Asserts that the change stream cursor given by 'cursor' returns at least one 'dropType'
     * notification before returning the next notification given by 'expectedNext'. If running in a
     * sharded passthrough suite, the expectation is to receive a 'dropType' notification from each
     * shard that has at least one chunk. If the change stream is watching the single collection,
     * then the first drop will invalidate the stream.
     *
     * Returns an array of documents which includes all drop events consumed and the expected change
     * itself.
     */
    self.consumeDropUpTo = function({cursor, dropType, expectedNext, expectInvalidate}) {
        expectInvalidate = expectInvalidate || false;

        let results = [];
        let change = self.getOneChange(cursor, expectInvalidate);
        while (change.operationType == dropType) {
            results.push(change);
            change = self.getOneChange(cursor, expectInvalidate);
        }
        results.push(change);
        assertChangeIsExpected([expectedNext], 0, [change], expectInvalidate);

        return results;
    };

    /**
     * Asserts that the notifications from the change stream cursor include 0 or more 'drop'
     * notifications followed by a 'dropDatabase' notification.
     *
     * Returns the list of notifications.
     */
    self.assertDatabaseDrop = function({cursor, db}) {
        return self.consumeDropUpTo({
            cursor: cursor,
            dropType: "drop",
            expectedNext: {operationType: "dropDatabase", ns: {db: db.getName()}}
        });
    };
}

/**
 * Asserts that the given pipeline will eventually return an error with the provided code, either in
 * the initial aggregate, or a subsequent getMore. Throws an exception if there are any results from
 * running the pipeline, or if it doesn't throw an error within the window of assert.soon(). If
 * 'doNotModifyInPassthroughs' is 'true' and the test is running in a $changeStream upconversion
 * passthrough, then this stream will not be modified and will run as though no passthrough were
 * active.
 */
ChangeStreamTest.assertChangeStreamThrowsCode = function assertChangeStreamThrowsCode(
    {db, collName, pipeline, expectedCode, doNotModifyInPassthroughs}) {
    try {
        // Run a passthrough-aware initial 'aggregate' command to open the change stream.
        const res = assert.commandWorked(runCommandChangeStreamPassthroughAware(
            db,
            {aggregate: collName, pipeline: pipeline, cursor: {batchSize: 1}},
            doNotModifyInPassthroughs));

        // Create a cursor using the command response, and begin issuing getMores. We expect
        // csCursor.hasNext() to throw the expected code before assert.soon() times out.
        const csCursor = new DBCommandCursor(db, res, 1);
        assert.soon(() => csCursor.hasNext());
        assert(false, `Unexpected result from cursor: ${tojsonMaybeTruncate(csCursor.next())}`);
    } catch (error) {
        assert.eq(
            error.code, expectedCode, `Caught unexpected error: ${tojsonMaybeTruncate(error)}`);
        return true;
    }
    assert(false, "expected this to be unreachable");
};

/**
 * Static method for determining which database to run the change stream aggregation on based on
 * the watchMode.
 */
ChangeStreamTest.getDBForChangeStream = function(watchMode, dbObj) {
    if (watchMode == ChangeStreamWatchMode.kCluster) {
        return dbObj.getSiblingDB("admin");
    }
    return dbObj;
};

/**
 * A set of functions to help validate the behaviour of $changeStreams for a given namespace.
 */
function assertChangeStreamNssBehaviour(dbName, collName = "test", options, assertFunc) {
    const testDb = db.getSiblingDB(dbName);
    options = (options || {});
    const res = testDb.runCommand(
        Object.assign({aggregate: collName, pipeline: [{$changeStream: options}], cursor: {}}));
    return assertFunc(res);
}
function assertValidChangeStreamNss(dbName, collName = "test", options) {
    const res = assertChangeStreamNssBehaviour(dbName, collName, options, assert.commandWorked);
    assert.commandWorked(db.getSiblingDB(dbName).runCommand(
        {killCursors: (collName == 1 ? "$cmd.aggregate" : collName), cursors: [res.cursor.id]}));
}
function assertInvalidChangeStreamNss(dbName, collName = "test", options) {
    assertChangeStreamNssBehaviour(
        dbName,
        collName,
        options,
        (res) => assert.commandFailedWithCode(
            res, [ErrorCodes.InvalidNamespace, ErrorCodes.InvalidOptions]));
}

const kPreImagesCollectionDatabase = "config";
const kPreImagesCollectionName = "system.preimages";

/**
 * Asserts that 'changeStreamPreAndPostImages' collection option is present and is enabled for
 * collection.
 */
function assertChangeStreamPreAndPostImagesCollectionOptionIsEnabled(db, collName) {
    const collectionInfos = db.getCollectionInfos({name: collName});
    assert(collectionInfos[0].options["changeStreamPreAndPostImages"]["enabled"] === true);
}

/**
 * Asserts that 'changeStreamPreAndPostImages' collection option is absent in the collection.
 */
function assertChangeStreamPreAndPostImagesCollectionOptionIsAbsent(db, collName) {
    const collectionInfos = db.getCollectionInfos({name: collName});
    assert(!collectionInfos[0].options.hasOwnProperty("changeStreamPreAndPostImages"));
}

function getPreImagesCollection(db) {
    return db.getSiblingDB(kPreImagesCollectionDatabase).getCollection(kPreImagesCollectionName);
}

// Returns the pre-images written while performing the write operations.
function preImagesForOps(db, writeOps) {
    const preImagesColl = getPreImagesCollection(db);
    const preImagesCollSortSpec = {"_id.ts": 1, "_id.applyOpsIndex": 1};

    // Determine the id of the last pre-image document written to be able to determine the pre-image
    // documents written by 'writeOps()'. The pre-image purging job may concurrently remove some
    // pre-image documents while this function is executing.
    const preImageIdsBefore =
        preImagesColl.find({}, {}).sort(preImagesCollSortSpec).allowDiskUse().toArray();
    const lastPreImageId = (preImageIdsBefore.length > 0)
        ? preImageIdsBefore[preImageIdsBefore.length - 1]._id
        : undefined;

    // Perform the write operations.
    writeOps();

    // Return only newly written pre-images.
    const preImageFilter = lastPreImageId ? {"_id.ts": {$gt: lastPreImageId.ts}} : {};
    const result =
        preImagesColl.find(preImageFilter).sort(preImagesCollSortSpec).allowDiskUse().toArray();

    // Verify that the result is correct by checking if the last pre-image still exists. However, if
    // no pre-image document existed before 'writeOps()' invocation, the result may be incorrect.
    assert(lastPreImageId === undefined || preImagesColl.find({_id: lastPreImageId}).itcount() == 1,
           "Last pre-image document has been removed by the pre-image purging job.");
    return result;
}

/**
 * Returns documents from the pre-images collection from 'connection' ordered by _id.ts,
 * _id.applyOpsIndex ascending.
 */
function getPreImages(connection) {
    return connection.getDB(kPreImagesCollectionDatabase)[kPreImagesCollectionName]
        .find()
        .sort({"_id.ts": 1, "_id.applyOpsIndex": 1})
        .allowDiskUse()
        .toArray();
}

function findPreImagesCollectionDescriptions(db) {
    return db.getSiblingDB(kPreImagesCollectionDatabase).runCommand("listCollections", {
        filter: {name: kPreImagesCollectionName}
    });
}

/**
 * Asserts that pre-images collection is absent in configDB.
 */
function assertPreImagesCollectionIsAbsent(db) {
    const result = findPreImagesCollectionDescriptions(db);
    assert.eq(result.cursor.firstBatch.length, 0);
}

/**
 * Asserts that pre-images collection is created in the configDB and has clustered index on _id.
 */
function assertPreImagesCollectionExists(db) {
    const collectionInfos = findPreImagesCollectionDescriptions(db);
    assert.eq(collectionInfos.cursor.firstBatch.length, 1, collectionInfos);
    const preImagesCollectionDescription = collectionInfos.cursor.firstBatch[0];
    assert.eq(preImagesCollectionDescription.name, "system.preimages");

    // Verifies that the pre-images collection is clustered by _id.
    assert(preImagesCollectionDescription.hasOwnProperty("options"),
           preImagesCollectionDescription);
    assert(preImagesCollectionDescription.options.hasOwnProperty("clusteredIndex"),
           preImagesCollectionDescription);
    const clusteredIndexDescription = preImagesCollectionDescription.options.clusteredIndex;
    assert(clusteredIndexDescription, preImagesCollectionDescription);
}
