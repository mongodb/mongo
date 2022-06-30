/**
 * Tests the behavior of change streams in the presence of 'showExpandedEvents' flag.
 *
 * @tags: [
 *   requires_fcv_61,
 *   # The test assumes certain ordering of the events. The chunk migrations on a sharded collection
 *   # could break the test.
 *   assumes_unsharded_collection,
 *   featureFlagChangeStreamsFurtherEnrichedEvents,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/collection_drop_recreate.js');  // For 'assertDropAndRecreateCollection' and
                                                   // 'assertDropCollection'.
load('jstests/libs/change_stream_util.js');        // For 'ChangeStreamTest' and
                                                   // 'assertChangeStreamEventEq'.

const testDB = db.getSiblingDB(jsTestName());

// Assert that the flag is not allowed with 'apiStrict'.
assert.commandFailedWithCode(testDB.runCommand({
    aggregate: 1,
    pipeline: [{$changeStream: {showExpandedEvents: true}}],
    cursor: {},
    apiVersion: "1",
    apiStrict: true
}),
                             ErrorCodes.APIStrictError);

assert.commandWorked(testDB.runCommand(
    {aggregate: 1, pipeline: [{$changeStream: {showExpandedEvents: true}}], cursor: {}}));

const dbName = testDB.getName();
const collName = 'enriched_events';
const renamedCollName = 'enriched_events_renamed';
const ns = {
    db: dbName,
    coll: collName
};
const renamedNs = {
    db: dbName,
    coll: renamedCollName,
};
const coll = assertDropAndRecreateCollection(testDB, collName);

const pipeline = [{$changeStream: {showExpandedEvents: true}}];
const test = new ChangeStreamTest(testDB);
const openChangeStreamCursor = () => test.startWatchingChanges({pipeline, collection: 1});
let cursor = openChangeStreamCursor();

function assertNextChangeEvent(expectedEvent, checkUuid = true) {
    const event = test.getOneChange(cursor);

    // Check that 'collectionUUID' field is absent if we are not expecting it.
    assert(checkUuid || event.collectionUUID === undefined);

    // Check the presence and the type of 'wallTime' field. We have no way to check the correctness
    // of 'wallTime' value, so we delete it afterwards.
    assert(event.wallTime instanceof Date);
    delete event.wallTime;

    assertChangeStreamEventEq(event, expectedEvent);
}

function getCollectionUuid(coll) {
    const collInfo = testDB.getCollectionInfos({name: coll})[0];
    return collInfo.info.uuid;
}

function assertChangeEvent(operation, expectedEvent, checkUuid = true) {
    if (checkUuid) {
        expectedEvent.collectionUUID = getCollectionUuid(expectedEvent.ns.coll);
    }

    operation();

    assertNextChangeEvent(expectedEvent, checkUuid);
}

// Test change stream event for 'insert' operation.
assertChangeEvent(() => assert.commandWorked(coll.insert({_id: 0, a: 1})), {
    ns,
    operationType: 'insert',
    documentKey: {_id: 0},
    fullDocument: {_id: 0, a: 1},
});

// Test change stream event for replacement-style 'update' operation.
assertChangeEvent(() => assert.commandWorked(coll.update({_id: 0}, {_id: 0, a: 2})), {
    ns,
    operationType: 'replace',
    documentKey: {_id: 0},
    fullDocument: {_id: 0, a: 2},
});

// Test change stream event for regular 'update' operation.
assertChangeEvent(() => assert.commandWorked(coll.update({_id: 0}, {$inc: {a: 1}})), {
    ns,
    operationType: 'update',
    documentKey: {_id: 0},
    updateDescription:
        {removedFields: [], updatedFields: {a: 3}, truncatedArrays: [], disambiguatedPaths: {}},
});

// Test change stream event for 'remove' operation.
assertChangeEvent(() => assert.commandWorked(coll.remove({_id: 0})), {
    ns,
    operationType: 'delete',
    documentKey: {_id: 0},
});

// Test change stream event for 'drop' operation.
assertChangeEvent(() => assertDropCollection(testDB, collName), {
    ns,
    operationType: 'drop',
});
assertCreateCollection(testDB, collName);
cursor = openChangeStreamCursor();

// Test change stream event for 'rename' operation with 'dropTarget: false'.
assertChangeEvent(() => assert.commandWorked(coll.renameCollection(renamedCollName)), {
    ns,
    operationType: 'rename',
    to: renamedNs,
    operationDescription: {
        to: renamedNs,
    },
});
assertDropCollection(testDB, renamedCollName);
assertCreateCollection(testDB, collName);
cursor = openChangeStreamCursor();

// Test change stream event for 'rename' operation with 'dropTarget: true' when target collection
// does not exist.
assertChangeEvent(() => assert.commandWorked(coll.renameCollection(renamedCollName, true)), {
    ns,
    operationType: 'rename',
    to: renamedNs,
    operationDescription: {
        to: renamedNs,
    },
});
assertDropCollection(testDB, renamedCollName);
assertCreateCollection(testDB, collName);
cursor = openChangeStreamCursor();

// Test change stream event for 'rename' operation with 'dropTarget: true' when target collection
// exists.
assertCreateCollection(testDB, renamedCollName);
assertNextChangeEvent({
    ns: renamedNs,
    operationType: 'create',
    operationDescription: {idIndex: {v: 2, key: {_id: 1}, name: "_id_"}}

});
assertChangeEvent(() => assert.commandWorked(coll.renameCollection(renamedCollName, true)), {
    ns,
    operationType: 'rename',
    to: renamedNs,
    operationDescription: {
        dropTarget: getCollectionUuid(renamedCollName),
        to: renamedNs,
    },
});
assertDropCollection(testDB, renamedCollName);

// Test change stream event for 'dropDatabase' operation.
cursor = test.startWatchingChanges({pipeline, collection: 1});
assertChangeEvent(
    () => assert.commandWorked(testDB.dropDatabase()),
    {
        ns: {
            db: dbName,
        },
        operationType: 'dropDatabase',
    },
    false /* checkUuid */
);
}());
