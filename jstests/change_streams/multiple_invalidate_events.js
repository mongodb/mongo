/** Test cases to verify the resumabilty of change stream when multiple invalidate events are in the
 * stream.
 * @tags: [
 * assumes_read_preference_unchanged,
 * uses_change_streams,
 * do_not_run_in_whole_cluster_passthrough,
 * do_not_run_in_whole_db_passthrough
 * ]
 */
import {assertCreateCollection, assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {describe, it, afterEach} from "jstests/libs/mochalite.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

describe("changeStream resumability with multiple invalidate events", () => {
    const testDB = db.getSiblingDB(jsTestName());
    const kCollName = "test";

    const kMatchStage = {
        // Ignore certain DDL event types that can be inserted by passthroughs.
        $match: {operationType: {$nin: ["createIndexes", "dropIndexes", "shardCollection"]}},
    };

    const buildPipeline = (resumeToken = undefined) => {
        let changeStreamStage = {$changeStream: {showExpandedEvents: true}};
        if (resumeToken) {
            changeStreamStage.$changeStream.startAfter = resumeToken;
        }
        return [changeStreamStage, kMatchStage];
    };

    const openChangeStream = ({collection, resumeToken = undefined}) => {
        if (csTest) {
            csTest.cleanUp();
        }
        csTest = new ChangeStreamTest(testDB);
        return csTest.startWatchingChanges({
            pipeline: buildPipeline(resumeToken),
            collection,
        });
    };

    const kCollectionCreateEvent = {
        operationType: "create",
        ns: {db: testDB.getName(), coll: kCollName},
        nsType: "collection",
    };

    const kCollectionDropEvent = {
        operationType: "drop",
        ns: {db: testDB.getName(), coll: kCollName},
    };

    const kInsertDocId0Event = {
        operationType: "insert",
        fullDocument: {_id: 0, a: 1},
        ns: {db: testDB.getName(), coll: kCollName},
        documentKey: {_id: 0},
    };

    const kInsertDocId1Event = {
        operationType: "insert",
        fullDocument: {_id: 1, a: 2},
        ns: {db: testDB.getName(), coll: kCollName},
        documentKey: {_id: 1},
    };

    const kInsertDocId2Event = {
        operationType: "insert",
        fullDocument: {_id: 2, a: 3},
        ns: {db: testDB.getName(), coll: kCollName},
        documentKey: {_id: 2},
    };

    const kInsertDoc3Event = {
        operationType: "insert",
        fullDocument: {_id: 3, a: 4},
        ns: {db: testDB.getName(), coll: kCollName},
        documentKey: {_id: 3},
    };

    const kDropDatabaseEvent = {
        operationType: "dropDatabase",
        ns: {db: testDB.getName()},
    };

    const kInvalidateEvent = {
        operationType: "invalidate",
    };

    let csTest;

    afterEach(() => {
        if (csTest) {
            csTest.cleanUp();
            csTest = null;
        }

        testDB.dropDatabase();
    });

    it("can resume collection-level change stream from multiple drop events", () => {
        let csCursor = openChangeStream({collection: kCollName});
        csTest.assertNoChange(csCursor);

        // Create collection and insert one document.
        const coll = assertDropAndRecreateCollection(testDB, kCollName);
        assert.commandWorked(coll.insert({_id: 0, a: 1}));
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [kCollectionCreateEvent, kInsertDocId0Event],
        });

        // Drop the collection. Expect "drop" and "invalidate" events in the stream.
        coll.drop();
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [kCollectionDropEvent, kInvalidateEvent],
            expectInvalidate: true,
        });

        // Retrieve the resume token from the stream, and close the change stream.
        let resumeToken = csTest.getResumeToken(csCursor);

        // Recreate the collection and insert a new document.
        assertCreateCollection(testDB, kCollName);
        assert.commandWorked(coll.insert({_id: 1, a: 2}));

        // Drop the collection again.
        coll.drop();

        // Resume the change stream from the invalidate event.
        // Expect "create", "insert", "drop" and "invalidate" events in the stream.
        csCursor = openChangeStream({collection: kCollName, resumeToken});
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [kCollectionCreateEvent, kInsertDocId1Event, kCollectionDropEvent, kInvalidateEvent],
            expectInvalidate: true,
        });

        resumeToken = csTest.getResumeToken(csCursor);

        // Recreate the collection a third time and insert a new document.
        assertCreateCollection(testDB, kCollName);
        assert.commandWorked(coll.insert({_id: 2, a: 3}));

        // Drop the entire database. This will first drop the collection.
        testDB.dropDatabase();

        // Resume the change stream from the invalidate event, and expect "create", "insert", "drop"
        // and "invalidate" events in the stream.
        csCursor = openChangeStream({collection: kCollName, resumeToken});
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [kCollectionCreateEvent, kInsertDocId2Event, kCollectionDropEvent, kInvalidateEvent],
            expectInvalidate: true,
        });

        resumeToken = csTest.getResumeToken(csCursor);

        // Recreate the collection and insert a new document.
        assertCreateCollection(testDB, kCollName);
        assert.commandWorked(coll.insert({_id: 3, a: 4}));

        // Resume the change stream from the invalidate event. Now expect "create" and "insert"
        // events in the stream, but no "drop" or "invalidate" events (as the collection still
        // exists).
        csCursor = openChangeStream({collection: kCollName, resumeToken});
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [kCollectionCreateEvent, kInsertDoc3Event],
            expectInvalidate: false,
        });

        csTest.assertNoChange(csCursor);
    });

    it("can resume database-level change stream from multiple drop events", () => {
        let csCursor = openChangeStream({collection: 1});
        csTest.assertNoChange(csCursor);

        // Create collection and insert one document.
        const coll = assertDropAndRecreateCollection(testDB, kCollName);
        assert.commandWorked(coll.insert({_id: 0, a: 1}));

        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [kCollectionCreateEvent, kInsertDocId0Event],
        });

        // Drop the database.
        testDB.dropDatabase();

        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [kCollectionDropEvent, kDropDatabaseEvent, kInvalidateEvent],
            expectInvalidate: true,
        });

        // Retrieve the resume token from the stream, and close the change stream.
        let resumeToken = csTest.getResumeToken(csCursor);

        // Recreate the collection and insert a new document.
        assertCreateCollection(testDB, kCollName);
        assert.commandWorked(coll.insert({_id: 1, a: 2}));

        // Drop the database again.
        testDB.dropDatabase();

        // Resume the change stream from the invalidate event. Expect "create", "insert", "drop",
        // "dropDatabase", and "invalidate" events in the stream.
        csCursor = openChangeStream({collection: 1, resumeToken});
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [
                kCollectionCreateEvent,
                kInsertDocId1Event,
                kCollectionDropEvent,
                kDropDatabaseEvent,
                kInvalidateEvent,
            ],
            expectInvalidate: true,
        });

        resumeToken = csTest.getResumeToken(csCursor);

        // Recreate the collection a third time and insert a new document.
        assertCreateCollection(testDB, kCollName);
        assert.commandWorked(coll.insert({_id: 2, a: 3}));

        // Resume the change stream from the invalidate event. Expect "create" and "insert", as the
        // collection/database still exists.
        csCursor = openChangeStream({collection: 1, resumeToken});
        csTest.assertNextChangesEqual({
            cursor: csCursor,
            expectedChanges: [kCollectionCreateEvent, kInsertDocId2Event],
            expectInvalidate: false,
        });

        csTest.assertNoChange(csCursor);
    });
});
