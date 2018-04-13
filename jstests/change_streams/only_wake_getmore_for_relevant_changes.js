// Test that an insert to an unrelated collection will not cause a $changeStream getMore to
// return early.
(function() {
    "use strict";

    load('jstests/libs/uuid_util.js');
    load("jstests/libs/fixture_helpers.js");           // For 'FixtureHelpers'.
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    /**
     * Uses a parallel shell to execute the javascript function 'event' at the same time as an
     * awaitData getMore on the cursor with id 'awaitDataCursorId'. Returns the result of the
     * getMore, and the time it took to complete.
     *
     * Note that 'event' will not have access to any local variables, since it will be executed in a
     * different scope.
     */
    function runGetMoreInParallelWithEvent(
        {collection, awaitDataCursorId, identifyingComment, maxTimeMS, event}) {
        // In some extreme cases, the parallel shell can take longer to start up than it takes for
        // the getMore to run. To prevent this from happening, the main thread waits for an insert
        // into "sentinel", to signal that the parallel shell has started and is waiting for the
        // getMore to appear in currentOp.
        const port =
            (collection.stats().sharded ? collection.getMongo().port
                                        : FixtureHelpers.getPrimaryForNodeHostingDatabase(db).port);

        const sentinelCountBefore = shellSentinelCollection.find().itcount();

        const awaitShellDoingEventDuringGetMore = startParallelShell(`
            // Wait for the getMore to appear in currentOp.
            assert.soon(function() {
                return db.currentOp({
                             op: "getmore",
                             "originatingCommand.comment": "${identifyingComment}",
                         }).inprog.length > 0;
            });
            const eventFn = ${event.toString()};
            eventFn();
            //Signal that the parallel shell has completed its event function.
            assert.writeOK(db.getCollection("${shellSentinelCollection.getName()}").insert({}));`,
                                                                     port);

        // Run and time the getMore.
        let startTime, result, elapsedMs;
        assert.soon(function() {
            startTime = (new Date()).getTime();
            result = assert.commandWorked(db.runCommand({
                getMore: awaitDataCursorId,
                collection: collection.getName(),
                maxTimeMS: maxTimeMS
            }));
            elapsedMs = (new Date()).getTime() - startTime;
            return result.cursor.nextBatch.length > 0 ||
                shellSentinelCollection.find().itcount() > sentinelCountBefore;
        });
        awaitShellDoingEventDuringGetMore();
        return {result: result, elapsedMs: elapsedMs};
    }

    /**
     * Asserts that a getMore of the cursor given by 'awaitDataCursorId' will not return after
     * 'event' is called, and will instead keep waiting until its maxTimeMS is expired.
     *
     * @param [Collection] collection - the collection to use in the getMore command.
     * @param [NumberLong] awaitDataCursorId - the id of the cursor to use in the getMore command.
     * @param [Function] event - the event that should be run during the getMore.
     */
    function assertEventDoesNotWakeCursor(
        {collection, awaitDataCursorId, identifyingComment, event}) {
        const {result, elapsedMs} = runGetMoreInParallelWithEvent({
            collection: collection,
            awaitDataCursorId: awaitDataCursorId,
            identifyingComment: identifyingComment,
            maxTimeMS: 1000,
            event: event,
        });
        // Should have waited for at least 'maxTimeMS'.
        assert.gt(elapsedMs, 900, "getMore returned before waiting for maxTimeMS");
        const cursorResponse = result.cursor;
        // Cursor should be valid with no data.
        assert.neq(cursorResponse.id, 0);
        assert.eq(cursorResponse.nextBatch.length, 0);
    }

    /**
     * Asserts that a getMore of the cursor given by 'awaitDataCursorId' will return soon after
     * 'event' is called, and returns the response from the getMore command.
     *
     * @param [Collection] collection - the collection to use in the getMore command.
     * @param [NumberLong] awaitDataCursorId - the id of the cursor to use in the getMore command.
     * @param [Function] event - the event that should be run during the getMore.
     */
    function assertEventWakesCursor({collection, awaitDataCursorId, identifyingComment, event}) {
        // Run the original event, then (while still in the parallel shell) assert that the getMore
        // finishes soon after. This will be run in a parallel shell, which will not have a variable
        // 'event' in scope, so we'll have to stringify it here.
        const thirtyMinutes = 30 * 60 * 1000;
        const fiveMinutes = 5 * 60 * 1000;
        const {result, elapsedMs} = runGetMoreInParallelWithEvent({
            collection: collection,
            awaitDataCursorId: awaitDataCursorId,
            identifyingComment: identifyingComment,
            maxTimeMS: thirtyMinutes,
            event: event,
        });

        assert.lt(elapsedMs, fiveMinutes);

        return result;
    }

    // Refresh all collections which will be required in the course of this test.
    const shellSentinelCollection = assertDropAndRecreateCollection(db, "shell_sentinel");
    const changesCollection = assertDropAndRecreateCollection(db, "changes");
    const unrelatedCollection = assertDropCollection(db, "unrelated_collection");

    // Start a change stream cursor.
    const wholeCollectionStreamComment = "change stream on entire collection";
    let res = assert.commandWorked(db.runCommand({
        aggregate: changesCollection.getName(),
        // Project out the resume token, since that's subject to change unpredictably.
        pipeline: [{$changeStream: {}}, {$project: {"_id": 0}}],
        cursor: {},
        comment: wholeCollectionStreamComment
    }));
    const changeCursorId = res.cursor.id;
    assert.neq(changeCursorId, 0);
    assert.eq(res.cursor.firstBatch.length, 0);

    // Test that an insert during a getMore will wake up the cursor and immediately return with the
    // new result.
    const getMoreResponse = assertEventWakesCursor({
        collection: changesCollection,
        awaitDataCursorId: changeCursorId,
        identifyingComment: wholeCollectionStreamComment,
        event: () => assert.writeOK(db.changes.insert({_id: "wake up"}))
    });
    assert.eq(getMoreResponse.cursor.nextBatch.length, 1);
    assert.eq(getMoreResponse.cursor.nextBatch[0].operationType,
              "insert",
              tojson(getMoreResponse.cursor.nextBatch[0]));
    assert.eq(getMoreResponse.cursor.nextBatch[0].fullDocument,
              {_id: "wake up"},
              tojson(getMoreResponse.cursor.nextBatch[0]));

    // Test that an insert to an unrelated collection will not cause the change stream to wake up
    // and return an empty batch before reaching the maxTimeMS.
    assertEventDoesNotWakeCursor({
        collection: changesCollection,
        awaitDataCursorId: changeCursorId,
        identifyingComment: wholeCollectionStreamComment,
        event: () => assert.writeOK(db.unrelated_collection.insert({_id: "unrelated change"}))
    });
    assert.commandWorked(
        db.runCommand({killCursors: changesCollection.getName(), cursors: [changeCursorId]}));

    // Test that changes ignored by filtering in later stages of the pipeline will not cause the
    // cursor to return before the getMore has exceeded maxTimeMS.
    const noInvalidatesComment = "change stream filtering invalidate entries";
    res = assert.commandWorked(db.runCommand({
        aggregate: changesCollection.getName(),
        // This pipeline filters changes to only invalidates, so regular inserts should not cause
        // the awaitData to end early.
        pipeline: [{$changeStream: {}}, {$match: {operationType: "invalidate"}}],
        cursor: {},
        comment: noInvalidatesComment
    }));
    assert.eq(
        res.cursor.firstBatch.length, 0, "did not expect any invalidations on changes collection");
    assert.neq(res.cursor.id, 0);
    assertEventDoesNotWakeCursor({
        collection: changesCollection,
        awaitDataCursorId: res.cursor.id,
        identifyingComment: noInvalidatesComment,
        event: () => assert.writeOK(db.changes.insert({_id: "should not appear"}))
    });
    assert.commandWorked(
        db.runCommand({killCursors: changesCollection.getName(), cursors: [res.cursor.id]}));
}());
