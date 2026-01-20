/**
 * Test that change streams returns DDL operation on views.
 *
 * @tags: [
 * ]
 */
import {assertCreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    assertChangeStreamEventEq,
    canonicalizeEventForTesting,
    ChangeStreamTest,
} from "jstests/libs/query/change_stream_util.js";
import {isRawOperationSupported} from "jstests/libs/raw_operation_utils.js";
import {describe, afterEach, it} from "jstests/libs/mochalite.js";

const testDB = db.getSiblingDB(jsTestName());

const dbName = testDB.getName();
const viewPipeline = [{$match: {a: 2}}, {$project: {a: 1}}];

function runViewEventAndResumeTest(showSystemEvents) {
    jsTest.log.info("runViewEventAndResumeTest(showSystemEvents=" + showSystemEvents + ")");

    // Drop all the namespaces accessed in the test.
    assertDropCollection(testDB, "base");
    assertDropCollection(testDB, "view");
    assertDropCollection(testDB, "viewOnView");
    // Drop also the database itself (to drop system collections)
    assert.commandWorked(testDB.dropDatabase());

    // Insert some data on the base collection so that the passthrough suites would finishing
    // setting up the collections, and does not generate unexpected operations during the test.
    assert.commandWorked(testDB.createCollection("base"));
    assert.commandWorked(testDB["base"].insert({_id: 1}));

    let cursor = testDB.aggregate([{$changeStream: {showExpandedEvents: true, showSystemEvents: showSystemEvents}}]);

    assert.commandWorked(testDB.createView("view", "base", viewPipeline));
    assert.soon(() => cursor.hasNext());

    let event = cursor.next();

    if (showSystemEvents) {
        // Creating the first view of the database also creates `system.views`.
        assert(event.clusterTime, event);
        assert(event.wallTime, event);
        assertChangeStreamEventEq(event, {
            operationType: "create",
            ns: {db: dbName, coll: "system.views"},
            nsType: "collection",
        });

        assert.soon(() => cursor.hasNext());
        event = cursor.next();
    }

    assert(event.clusterTime, event);
    assert(event.wallTime, event);
    assertChangeStreamEventEq(event, {
        operationType: "create",
        ns: {db: dbName, coll: "view"},
        operationDescription: {viewOn: "base", pipeline: viewPipeline},
        nsType: "view",
    });

    // Ensure that we can resume the change stream using a resuming token from the create view
    // event.
    cursor = testDB.aggregate([
        {
            $changeStream: {resumeAfter: event._id, showExpandedEvents: true, showSystemEvents: showSystemEvents},
        },
    ]);
    assert.commandWorked(testDB.createView("viewOnView", "view", viewPipeline));
    assert.soon(() => cursor.hasNext());

    const events = [];
    const createEvent = cursor.next();
    events.push(createEvent);

    assertChangeStreamEventEq(createEvent, {
        operationType: "create",
        ns: {db: dbName, coll: "viewOnView"},
        operationDescription: {viewOn: "view", pipeline: viewPipeline},
        nsType: "view",
    });

    // Test 'collMod' command on views.
    const updatedViewPipeline = [{$match: {a: 1}}, {$project: {a: 1}}];
    assert.commandWorked(testDB.runCommand({collMod: "viewOnView", viewOn: "view", pipeline: updatedViewPipeline}));
    assert.soon(() => cursor.hasNext());
    const modifyEvent = cursor.next();
    events.push(modifyEvent);

    assertChangeStreamEventEq(modifyEvent, {
        operationType: "modify",
        ns: {db: dbName, coll: "viewOnView"},
        operationDescription: {viewOn: "view", pipeline: updatedViewPipeline},
    });

    // Test 'drop' events.
    assertDropCollection(testDB, "view");
    assert.soon(() => cursor.hasNext());
    const dropEvent = cursor.next();
    events.push(dropEvent);

    assertChangeStreamEventEq(dropEvent, {operationType: "drop", ns: {db: dbName, coll: "view"}});

    assertDropCollection(testDB, "viewOnView");
    assert.soon(() => cursor.hasNext());
    const dropEventView = cursor.next();
    events.push(dropEventView);

    assertChangeStreamEventEq(dropEventView, {operationType: "drop", ns: {db: dbName, coll: "viewOnView"}});

    // Generate a dummy event so that we can test all events for resumability.
    assert.commandWorked(testDB.createView("dummyView", "view", viewPipeline));
    assert.soon(() => cursor.hasNext());

    const dummyEvent = cursor.next();
    events.push(dummyEvent);

    assertDropCollection(testDB, "dummyView");
    assert.soon(() => cursor.hasNext());

    // Test that for all the commands we can resume the change stream using a resume token.
    for (let idx = 0; idx < events.length - 1; idx++) {
        const event = events[idx];
        const subsequent = events[idx + 1];
        const newCursor = testDB.aggregate([
            {
                $changeStream: {
                    resumeAfter: event._id,
                    showExpandedEvents: true,
                    showSystemEvents: showSystemEvents,
                },
            },
        ]);
        assert.soon(() => newCursor.hasNext());
        assertChangeStreamEventEq(newCursor.next(), subsequent);
    }
}

function runViewEventForTsCollectionTest(showSystemEvents) {
    const events = [];
    let cursor = testDB.aggregate([{$changeStream: {showExpandedEvents: true, showSystemEvents: showSystemEvents}}]);

    // Test view change stream events on a timeseries collection.
    assert.commandWorked(testDB.createCollection("timeseries_coll", {timeseries: {timeField: "time"}}));

    if (showSystemEvents) {
        // Also expect the creation of the buckets collection.
        assert.soon(() => cursor.hasNext());
        const createBucketsEvent = cursor.next();
        events.push(createBucketsEvent);
        assertChangeStreamEventEq(createBucketsEvent, {
            operationType: "create",
            ns: {db: dbName, coll: "system.buckets.timeseries_coll"},
            nsType: "collection",
        });
    }

    assert.soon(() => cursor.hasNext());
    const createTimeseriesEvent = cursor.next();
    events.push(createTimeseriesEvent);

    assertChangeStreamEventEq(createTimeseriesEvent, {
        operationType: "create",
        ns: {db: dbName, coll: "timeseries_coll"},
        operationDescription: {
            viewOn: "system.buckets.timeseries_coll",
            pipeline: [{$_internalUnpackBucket: {timeField: "time", bucketMaxSpanSeconds: 3600}}],
        },
        nsType: "timeseries",
    });

    assert.commandWorked(testDB.runCommand({collMod: "timeseries_coll", timeseries: {granularity: "minutes"}}));
    assert.soon(() => cursor.hasNext());
    let modifyTimeseriesEvent = cursor.next();
    events.push(modifyTimeseriesEvent);

    // In some suites, the timeseries is sharded, so expect createIndexes and shardCollection
    if (showSystemEvents && modifyTimeseriesEvent.operationType == "createIndexes") {
        const createIndexesEvent = modifyTimeseriesEvent;

        assert.soon(() => cursor.hasNext());
        const shardCollectionEvent = cursor.next();
        events.push(shardCollectionEvent);

        assert.soon(() => cursor.hasNext());
        modifyTimeseriesEvent = cursor.next();
        events.push(modifyTimeseriesEvent);

        assertChangeStreamEventEq(createIndexesEvent, {
            operationType: "createIndexes",
            ns: {db: dbName, coll: "system.buckets.timeseries_coll"},
        });

        assertChangeStreamEventEq(shardCollectionEvent, {
            operationType: "shardCollection",
            ns: {db: dbName, coll: "system.buckets.timeseries_coll"},
        });
    }

    assertChangeStreamEventEq(modifyTimeseriesEvent, {
        operationType: "modify",
        ns: {db: dbName, coll: "timeseries_coll"},
        operationDescription: {
            viewOn: "system.buckets.timeseries_coll",
            pipeline: [{$_internalUnpackBucket: {timeField: "time", bucketMaxSpanSeconds: 86400}}],
        },
    });

    if (showSystemEvents) {
        // Modifying the timeseries also generates an update of the buckets collection.
        assert.soon(() => cursor.hasNext());
        const modifyBucketsEvent = cursor.next();
        events.push(modifyBucketsEvent);
        assertChangeStreamEventEq(
            modifyBucketsEvent,
            {operationType: "modify", ns: {db: dbName, coll: "system.buckets.timeseries_coll"}},
            (event, expected) => {
                event = canonicalizeEventForTesting(event, expected);
                delete event.stateBeforeChange;
                return event;
            },
        );
    }

    assertDropCollection(testDB, "timeseries_coll");

    assert.soon(() => cursor.hasNext());
    const dropTimeseriesEvent = cursor.next();
    events.push(dropTimeseriesEvent);

    assertChangeStreamEventEq(dropTimeseriesEvent, {
        operationType: "drop",
        ns: {db: dbName, coll: "timeseries_coll"},
    });

    if (showSystemEvents) {
        // Also expect the drop of the buckets collection.
        assert.soon(() => cursor.hasNext());
        const dropBucketsEvent = cursor.next();
        events.push(dropBucketsEvent);
        assertChangeStreamEventEq(dropBucketsEvent, {
            operationType: "drop",
            ns: {db: dbName, coll: "system.buckets.timeseries_coll"},
        });

        // Test that dropping system.views generates an event under `showSystemEvents`.
        assertDropCollection(testDB, "system.views");
        assert.soon(() => cursor.hasNext());
        const dropSystemViewsEvent = cursor.next();
        events.push(dropSystemViewsEvent);
        assertChangeStreamEventEq(dropSystemViewsEvent, {
            operationType: "drop",
            ns: {db: dbName, coll: "system.views"},
        });
    }
}

describe("$changeStream", function () {
    describe("can emit view DDL events", function () {
        it("runViewEventAndResumeTest without showSystemEvents", function () {
            runViewEventAndResumeTest(false /* showSystemEvents */);
        });

        it("runViewEventAndResumeTest with showSystemEvents", function () {
            runViewEventAndResumeTest(true /* showSystemEvents */);
        });
    });

    describe("can emit view DDL events for timeseries collections", function () {
        if (isRawOperationSupported(db)) {
            jsTest.log.info(
                "If raw operations are supported, skipping running tests as timeseries collection will not be implemented via view",
            );
            return;
        }

        it("runViewEventAndResumeTestForTsCollection without showSystemEvents", function () {
            runViewEventForTsCollectionTest(false /* showSystemEvents */);
        });

        it("runViewEventAndResumeTestForTsCollection with showSystemEvents", function () {
            runViewEventForTsCollectionTest(true /* showSystemEvents */);
        });
    });

    const cst = new ChangeStreamTest(testDB);

    afterEach(function () {
        assertDropCollection(testDB, "view");
        assertDropCollection(testDB, "base");
        cst.cleanUp();
    });

    it("can not be opened on views", function () {
        // Cannot start a change stream on a view namespace.
        assert.commandWorked(testDB.createView("view", "base", viewPipeline));
        assert.soon(() => {
            try {
                cst.startWatchingChanges({
                    pipeline: [{$changeStream: {showExpandedEvents: true}}],
                    collection: "view",
                    doNotModifyInPassthroughs: true,
                });
            } catch (e) {
                assert.commandFailedWithCode(e, ErrorCodes.CommandNotSupportedOnView);
                return true;
            }
            return false;
        });
    });

    it("creating and dropping a view with the same name as an existing collection does not emit view events", function () {
        let cursor = cst.startWatchingChanges({
            pipeline: [{$changeStream: {showExpandedEvents: true}}],
            collection: "view",
            doNotModifyInPassthroughs: true,
        });

        // Create a view, then drop it and create a normal collection with the same name.
        assert.commandWorked(testDB.createView("view", "base", viewPipeline));
        assertDropCollection(testDB, "view");
        assert.commandWorked(testDB.createCollection("view"));

        // Confirm that the stream only sees the normal collection creation, not the view events.
        const event = cst.getNextChanges(cursor, 1)[0];
        assert(event.collectionUUID, event);
        assertChangeStreamEventEq(event, {
            operationType: "create",
            ns: {db: dbName, coll: "view"},
            operationDescription: {idIndex: {v: 2, key: {_id: 1}, name: "_id_"}},
            nsType: "collection",
        });
    });

    it("view change events are not emitted for change streams opened on the underlying collection", function () {
        assertCreateCollection(testDB, "base");

        // Change stream on a single collection does not produce view events.
        const cursor = cst.startWatchingChanges({
            pipeline: [{$changeStream: {showExpandedEvents: true}}],
            collection: "base",
            doNotModifyInPassthroughs: true,
        });

        // Create a view on the base collection, then drop it and implicitly create a collection by inserting a document.
        assert.commandWorked(testDB.createView("view", "base", viewPipeline));
        assertDropCollection(testDB, "view");
        assert.commandWorked(testDB["base"].insert({_id: 0}));

        // Verify that the view related operations are ignored, and only the event for insert on the base
        // collection is returned.
        cst.assertNextChangesEqual({
            cursor,
            expectedChanges: [
                {
                    operationType: "insert",
                    ns: {db: dbName, coll: "base"},
                    fullDocument: {_id: 0},
                    documentKey: {_id: 0},
                },
            ],
        });
    });

    // Verify that collection names such as 'system_views' do not trigger fake view creation events.
    ["system_views", "systemxviews", "systemviews"].forEach((collName) => {
        it("creating collection with name " + collName + " does not emit view events", function () {
            assertCreateCollection(testDB, collName);

            // Note: excluding "createIndexes" and "shardCollection" events is necessary here because some
            // passthroughs add index creation events to the change stream.
            const cursor = cst.startWatchingChanges({
                pipeline: [
                    {$changeStream: {showExpandedEvents: true}},
                    {$match: {operationType: {$nin: ["shardCollection", "createIndexes"]}}},
                ],
                collection: 1,
                doNotModifyInPassthroughs: true,
            });

            // Insert a document into a collection with a name similar to 'system.views'.
            testDB[collName].insert({_id: "test"});

            // Verify that we only see the insert and no view creation event.
            cst.assertNextChangesEqual({
                cursor,
                expectedChanges: [
                    {
                        operationType: "insert",
                        ns: {db: dbName, coll: collName},
                        fullDocument: {_id: "test"},
                        documentKey: {_id: "test"},
                    },
                ],
            });
            cst.assertNoChange(cursor);

            assertDropCollection(testDB, collName);
        });
    });
});
