/*
 * A class with helper functions which operate on change streams. The class maintains a list of
 * opened cursors and kills them on cleanup.
 */

function ChangeStreamTest(_db, name = "ChangeStreamTest") {
    // Keeps track of cursors opened during the test so that we can be sure to
    // clean them up before the test completes.
    let _allCursors = [];
    let self = this;

    // Prevent accidental usages of the default db.
    const db = null;

    self.oplogProjection = {$project: {"_id": 0}};

    /**
     * Starts a change stream cursor with the given pipeline on the given collection. It uses
     * the 'aggregateOptions' if provided and elides the resume token if 'includeToken' is not set.
     * This saves the cursor so that it can be cleaned up later.
     *
     * Returns the cursor returned by the 'aggregate' command.
     */
    self.startWatchingChanges = function({pipeline, collection, includeToken, aggregateOptions}) {
        aggregateOptions = aggregateOptions || {};
        aggregateOptions.cursor = aggregateOptions.cursor || {batchSize: 1};

        if (!includeToken) {
            // Strip the oplog fields we aren't testing.
            pipeline.push(self.oplogProjection);
        }

        // The 'collection' argument may be either a collection name or DBCollection object.
        assert(collection instanceof DBCollection || typeof collection === "string");
        const collName = (collection instanceof DBCollection ? collection.getName() : collection);

        let res = assert.commandWorked(_db.runCommand(
            Object.merge({aggregate: collName, pipeline: pipeline}, aggregateOptions)));
        assert.neq(res.cursor.id, 0);
        _allCursors.push({db: _db.getName(), coll: collName, cursorId: res.cursor.id});
        return res.cursor;
    };

    /**
     * Issues a 'getMore' on the provided cursor and returns the cursor returned.
     */
    self.getNextBatch = function(cursor) {
        const collName = cursor.ns.split(/\.(.+)/)[1];
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
     * invalidated.
     */
    function assertChangeIsExpected(
        expectedChanges, numChangesSeen, observedChanges, expectInvalidate) {
        if (expectedChanges) {
            assert.docEq(observedChanges[numChangesSeen],
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
            assert.commandWorked(_db.getSiblingDB(testCursor.db).runCommand({
                killCursors: testCursor.coll,
                cursors: [testCursor.cursorId]
            }));
        }

    };
}

/**
 * Asserts that the given pipeline will eventually return an error with the provided code,
 * either in the initial aggregate, or a subsequent getMore. Throws an exception if there are
 * any results from running the pipeline, or if it doesn't throw an error within the window of
 * assert.soon().
 */
ChangeStreamTest.assertChangeStreamThrowsCode = function assertChangeStreamThrowsCode(
    {collection, pipeline, expectedCode}) {
    try {
        const changeStream = collection.aggregate(pipeline);
        assert.soon(() => changeStream.hasNext());
        assert(false, `Unexpected result from cursor: ${tojson(changeStream.next())}`);
    } catch (error) {
        assert.eq(error.code, expectedCode, `Caught unexpected error: ${tojson(error)}`);
        return true;
    }
    assert(false, "expected this to be unreachable");
};
