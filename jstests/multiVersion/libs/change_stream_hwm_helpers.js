/**
 * Helper functions and constants to support the change stream high water mark multiversion tests.
 */
const ChangeStreamHWMHelpers = (function() {
    /**
     * Specifies the exact version to be used in tests which require pre-backport 4.0 binaries.
     */
    const preBackport40Version = "4.0.5";
    const latest42Version = "latest";

    /**
     * Opens a stream on the given collection, and confirms that a PBRT is or is not produced based
     * on the value of 'expectPBRT'. The 'hwmToResume' argument is a high water mark from an earlier
     * upgraded/downgraded incarnation of the cluster, and 'expectResume' indicates whether or not
     * we should be able to resume from it. If 'expectPBRT' is true, we also generate a new high
     * water mark token, confirm that we can resume from it, and return it to the caller.
     */
    function testPostBatchAndHighWaterMarkTokens({coll, expectPBRT, hwmToResume, expectResume}) {
        // Log the test options for debugging.
        jsTestLog(tojsononeline({
            coll: coll.getFullName(),
            conn: coll.getMongo(),
            expectPBRT: expectPBRT,
            hwmToResume: hwmToResume,
            expectResume: expectResume
        }));

        // Verify that the command response object has a PBRT if 'expectPBRT' is true.
        const csCmdResponse = assert.commandWorked(coll.runCommand(
            {aggregate: coll.getName(), pipeline: [{$changeStream: {}}], cursor: {}}));
        assert.eq(expectPBRT, csCmdResponse.cursor.hasOwnProperty("postBatchResumeToken"));

        // Open a stream on the collection. If 'expectPBRT' is true then we should have a high water
        // mark token immediately.
        let csCursor = coll.watch();
        assert(!csCursor.hasNext());
        const newHWM = csCursor.getResumeToken();
        assert.eq(expectPBRT, newHWM != null);

        // Insert 10 documents into the test collection. If this is the sharded collection, we will
        // alternate between writing to each shard.
        for (let i = 0; i < 10; ++i) {
            const id = (i % 2 ? i : (-i));
            assert.commandWorked(coll.insert({_id: id}));
        }

        // Confirms that the expected _ids are seen by the change stream. If 'testResume' is true,
        // also verifies that we see the correct results when resuming/starting at each event.
        function assertChangeStreamEvents(cursor, startId, testResume) {
            for (let i = startId; i < 10; ++i) {
                const id = (i % 2 ? i : (-i));
                assert.soon(() => cursor.hasNext());
                const curRes = cursor.next();
                assert.eq(curRes.fullDocument._id, id);
                if (testResume) {
                    let resumeCursor = coll.watch([], {resumeAfter: curRes._id});
                    assertChangeStreamEvents(resumeCursor, (i + 1), false);
                    resumeCursor.close();
                    resumeCursor = coll.watch([], {startAtOperationTime: curRes.clusterTime});
                    assertChangeStreamEvents(resumeCursor, i, false);
                    resumeCursor.close();
                }
            }
        }

        // Verify that we can see all events, and can both resume after and start from each.
        assertChangeStreamEvents(csCursor, 0, true);

        // If we have a new high water mark token, confirm that we can resume after it.
        if (newHWM) {
            csCursor = coll.watch([], {resumeAfter: newHWM});
            assertChangeStreamEvents(csCursor, 0, false);
        }

        // If we were passed a HWM resume token from before the cluster was upgraded/downgraded,
        // verify that the resume behaviour is as expected.
        const invalidResumeTokenVersion = 50795;
        if (hwmToResume) {
            if (!expectResume) {
                // If we expect to fail the resume, confirm that we throw an assertion.
                assert.commandFailedWithCode(coll.runCommand({
                    aggregate: coll.getName(),
                    pipeline: [{$changeStream: {resumeAfter: hwmToResume}}],
                    cursor: {}
                }),
                                             invalidResumeTokenVersion);
            } else {
                // If we expect to be able to resume, confirm that we see the expected events.
                csCursor = coll.watch([], {resumeAfter: hwmToResume});
                assertChangeStreamEvents(csCursor, 0, false);
            }
        }

        // Remove all documents in the test collection so we start with a clean slate next time.
        assert.commandWorked(coll.remove({}));

        // Return the high water mark token from the start of the stream.
        return newHWM;
    }

    return {
        testPostBatchAndHighWaterMarkTokens: testPostBatchAndHighWaterMarkTokens,
        preBackport40Version: preBackport40Version,
        latest42Version: latest42Version,
    };
})();