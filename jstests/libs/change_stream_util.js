/*
 * A class with helper functions which operate on change streams. The class maintains a list of
 * opened cursors and kills them on cleanup.
 */

/**
 * Enumeration of the possible types of change streams.
 */
const ChangeStreamWatchMode = Object.freeze({
    kCollection: 1,
    kDb: 2,
    kCluster: 3,
});

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
 */
function assertInvalidateOp({cursor, opType}) {
    if (!isChangeStreamPassthrough() ||
        (changeStreamPassthroughType() == ChangeStreamWatchMode.kDb && opType == "dropDatabase")) {
        assert.soon(() => cursor.hasNext());
        assert.eq(cursor.next().operationType, "invalidate");
        assert(cursor.isExhausted());
        assert(cursor.isClosed());
    }
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
        assert.eq(nextBatch.length, 1, "Batch length wasn't 0 or 1: " + tojson(cursor));
        return nextBatch[0];
    }

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
            const lastObservedChange = Object.assign({}, observedChanges[numChangesSeen]);
            if (expectedChanges[numChangesSeen]._id == null) {
                delete lastObservedChange._id;  // Remove the resume token, if present.
            }
            if (expectedChanges[numChangesSeen].clusterTime == null) {
                delete lastObservedChange.clusterTime;  // Remove the cluster time, if present.
            }
            assert.docEq(lastObservedChange,
                         expectedChanges[numChangesSeen],
                         "Change did not match expected change. Expected changes: " +
                             tojson(expectedChanges));
        } else if (!expectInvalidate) {
            assert(!isInvalidated(observedChanges[numChangesSeen]),
                   "Change was invalidated when it should not have been. Number of changes seen: " +
                       numChangesSeen + ", observed changes: " + tojson(observedChanges) +
                       ", expected changes: " + tojson(expectedChanges));
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
    self.assertNextChangesEqual = function(
        {cursor, expectedChanges, expectedNumChanges, expectInvalidate, skipFirstBatch}) {
        expectInvalidate = expectInvalidate || false;
        skipFirstBatch = skipFirstBatch || false;

        // Assert that the expected length matches the length of the expected batch.
        if (expectedChanges !== undefined && expectedNumChanges !== undefined) {
            assert.eq(expectedChanges.length,
                      expectedNumChanges,
                      "Expected change's size must match expected number of changes");
        }

        // Set the expected number of changes based on the size of the expected change list.
        if (expectedNumChanges === undefined) {
            assert.neq(expectedChanges, undefined);
            expectedNumChanges = expectedChanges.length;
        }

        let changes = [];
        for (let i = 0; i < expectedNumChanges; i++) {
            // Since the first change may be on the original cursor, we need to check for that
            // change on the cursor before we move the cursor forward.
            if (i === 0 && !skipFirstBatch) {
                changes[0] = getNextDocFromCursor(cursor);
                if (changes[0]) {
                    assertChangeIsExpected(expectedChanges, 0, changes, expectInvalidate);
                    continue;
                }
            }

            assert.soon(function() {
                // We need to replace the cursor variable so we return the correct cursor.
                cursor = self.getNextBatch(cursor);
                changes[i] = getNextDocFromCursor(cursor);
                return changes[i] !== null;
            }, "timed out waiting for another result from the change stream");
            assertChangeIsExpected(expectedChanges, i, changes, expectInvalidate);
        }

        // If we expect invalidation, the final change should have operation type "invalidate".
        if (expectInvalidate) {
            assert(isInvalidated(changes[changes.length - 1]),
                   "Last change was not invalidated when it was expected: " + tojson(changes));

            // We make sure that the next batch kills the cursor after an invalidation entry.
            let finalCursor = self.getNextBatch(cursor);
            assert.eq(finalCursor.id, 0, "Final cursor was not killed: " + tojson(finalCursor));
        }

        return changes;
    };

    /**
     * Gets the next document in the change stream. This always executes a 'getMore' first.
     * If the current batch has a document in it, that one will be ignored.
     */
    self.getOneChange = function(cursor, expectInvalidate = false) {
        changes = self.assertNextChangesEqual({
            cursor: cursor,
            expectedNumChanges: 1,
            expectInvalidate: expectInvalidate,
            skipFirstBatch: true
        });
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
}

/**
 * Asserts that the given pipeline will eventually return an error with the provided code,
 * either in the initial aggregate, or a subsequent getMore. Throws an exception if there are
 * any results from running the pipeline, or if it doesn't throw an error within the window of
 * assert.soon().  If 'doNotModifyInPassthroughs' is 'true' and the test is running in a
 * $changeStream upconversion passthrough, then this stream will not be modified and will run as
 * though no passthrough were active.
 */
ChangeStreamTest.assertChangeStreamThrowsCode = function assertChangeStreamThrowsCode(
    {db, collName, pipeline, expectedCode, doNotModifyInPassthroughs}) {
    try {
        const res = assert.commandWorked(runCommandChangeStreamPassthroughAware(
            db,
            {aggregate: collName, pipeline: pipeline, cursor: {batchSize: 1}},
            doNotModifyInPassthroughs));

        // Extract the collection name from the cursor since the change stream may be on the whole
        // database. The 'collName' parameter will be the integer 1 in that case and the getMore
        // command requires 'collection' to be a string.
        const getMoreCollName = getCollectionNameFromFullNamespace(res.cursor.ns);
        assert.commandWorked(
            db.runCommand({getMore: res.cursor.id, collection: getMoreCollName, batchSize: 1}));
    } catch (error) {
        assert.eq(error.code, expectedCode, `Caught unexpected error: ${tojson(error)}`);
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
