/**
 * Reproduces the concurrent change-stream scenario from the C driver sample-commands test
 * (test_sample_change_stream_command), originally located at:
 * https://github.com/mongodb/mongo-c-driver/blob/71d593dc3cc649e75878482927a36259e9bf0fec/src/libmongoc/tests/test-mongoc-sample-commands.c#L2901
 *
 * A background shell continuously inserts empty documents while the test runs the four change
 * stream examples from the C driver sample. Examples 1-3 must succeed. Example 4 resumes from a
 * token using a pipeline that filters out the resume point, which the server rejects with a
 * ChangeStreamFatalError because the resume point becomes unreachable, so the test asserts that
 * failure and the accompanying error message.
 *
 * @tags: [
 *   assumes_read_preference_unchanged,
 *   change_stream_does_not_expect_txns,
 *   requires_fcv_91,
 *   requires_replication,
 *   uses_change_streams,
 *   uses_parallel_shell,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

describe("C driver sample change stream commands", function () {
    const testDB = db.getSiblingDB(jsTestName());
    const collName = jsTestName();
    const stopCollName = collName + "_stop";
    const coll = testDB.getCollection(collName);

    // Number of change events each example reads before it is considered successful.
    const kNumEventsToVerify = 3;

    // Verifies that 'event' is an insert of an empty document into 'coll'. The background writer
    // inserts {}, so the only field the server adds is the auto-generated '_id'.
    function verifyInsertEvent(event) {
        assert.eq(event.ns, {db: testDB.getName(), coll: collName}, "unexpected ns", {event});
        assert.eq(event.operationType, "insert", "unexpected operationType", {event});

        const fullDocument = Object.assign({}, event.fullDocument);
        delete fullDocument._id;
        assert.eq(fullDocument, {}, "fullDocument should only contain _id", {event});
    }

    // Reads kNumEventsToVerify change events from 'cs', verifying each one, and returns them.
    function readAndVerifyEvents(cs) {
        const events = [];
        for (let i = 0; i < kNumEventsToVerify; i++) {
            assert.soon(() => cs.hasNext(), "expected a change event");
            const event = cs.next();
            verifyInsertEvent(event);
            events.push(event);
        }
        return events;
    }

    // Returns the resume token of the last event read from 'events'. The token is the event's '_id';
    // cursor.getResumeToken() is deliberately not used because it returns the cursor's
    // high-water-mark post-batch resume token whenever the last event ended a batch. Resuming from
    // a high-water-mark token would not install the EnsureResumeTokenPresent stage, so example 4
    // would silently succeed instead of failing.
    function eventResumeToken(events) {
        const resumeToken = events[events.length - 1]._id;
        assert(resumeToken, "expected a resume token");
        return resumeToken;
    }

    before(function () {
        assertDropAndRecreateCollection(testDB, collName);
        assertDropAndRecreateCollection(testDB, stopCollName);

        // Background writer: insert an empty document every 100ms until the stop sentinel appears.
        this.awaitWriter = startParallelShell(
            funWithArgs(
                function (dbName, collName, stopCollName) {
                    const writerDB = db.getSiblingDB(dbName);
                    const writerColl = writerDB.getCollection(collName);
                    const stopColl = writerDB.getCollection(stopCollName);
                    while (true) {
                        assert.commandWorked(writerColl.insertOne({}));
                        if (stopColl.findOne() !== null) {
                            break;
                        }
                        sleep(100);
                    }
                },
                testDB.getName(),
                collName,
                stopCollName,
            ),
            db.getMongo().port,
        );
    });

    after(function () {
        assert.commandWorked(testDB.getCollection(stopCollName).insertOne({stop: true}));
        this.awaitWriter();
        assertDropCollection(testDB, collName);
        assertDropCollection(testDB, stopCollName);
    });

    // Opens a change stream, expecting to see a few events from the parallel writer.
    it("opens a change stream and reads events", function () {
        const cs = coll.watch();
        try {
            readAndVerifyEvents(cs);
        } finally {
            cs.close();
        }
    });

    it("opens a change stream with update lookup and reads events", function () {
        const cs = coll.watch([], {fullDocument: "updateLookup"});
        try {
            readAndVerifyEvents(cs);
        } finally {
            cs.close();
        }
    });

    // Opens a change stream, expecting to see a few events from the parallel writer, then opens
    // a second change stream and resumes from a resume token of the first stream.
    it("resumes a change stream after a resume token", function () {
        const cs = coll.watch();
        let resumeToken;
        try {
            resumeToken = eventResumeToken(readAndVerifyEvents(cs));
        } finally {
            cs.close();
        }

        const resumed = coll.watch([], {resumeAfter: resumeToken});
        try {
            readAndVerifyEvents(resumed);
        } finally {
            resumed.close();
        }
    });

    // Resumes a stream from another change stream, but with a different $match expression that
    // excludes the original stream's resume event.
    it("resuming while filtering out the resume point fails", function () {
        const cs = coll.watch();
        let resumeToken;
        try {
            resumeToken = eventResumeToken(readAndVerifyEvents(cs));
        } finally {
            cs.close();
        }

        // The background writer keeps inserting documents which do not match the filter below, so
        // the stream's high water mark advances past the resume point. Because the filter also
        // excludes the resume point itself, the server cannot prove the stream is resumable and
        // reports a fatal error.
        const pipeline = [{$match: {"fullDocument.username": "alice"}}];
        const error = assert.throwsWithCode(() => {
            const cs = coll.watch(pipeline, {resumeAfter: resumeToken});
            // The fatal error may be reported on a later getMore rather than when the stream
            // is opened, so keep polling until the server rejects the resume point.
            assert.soon(() => cs.hasNext());
        }, ErrorCodes.ChangeStreamFatalError);
        assert(
            error.message.includes("cannot resume stream"),
            "expected a 'cannot resume stream' error",
            {error: error.message},
        );
    });
});
