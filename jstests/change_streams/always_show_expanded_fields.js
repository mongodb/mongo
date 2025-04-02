/**
 * Verifies that change streams in 8.2 or higher return additional fields by default, without having
 * to use '{showExpandedEvents: true}'.
 *
 * @tags: [
 *   # The test assumes certain ordering of the events. The chunk migrations on a sharded collection
 *   # could break the test.
 *   assumes_unsharded_collection,
 *   requires_fcv_82
 * ]
 */
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {
    assertChangeStreamEventEq,
    ChangeStreamTest
} from "jstests/libs/query/change_stream_util.js";

const kLargeStr = 'x'.repeat(128);
const testDB = db.getSiblingDB(jsTestName());
const dbName = testDB.getName();
const collName = 'expanded_events_fields';
const ns = {
    db: dbName,
    coll: collName
};
const renamedCollName = 'enriched_events_renamed';
const renamedNs = {
    db: dbName,
    coll: renamedCollName,
};
const coll = assertDropAndRecreateCollection(testDB, collName);

const pipeline = [{$changeStream: {}}];
const test = new ChangeStreamTest(testDB);
const cursor = test.startWatchingChanges({pipeline, collection: 1});

function assertNextChangeEvent(expectedEvent) {
    const event = test.getOneChange(cursor);
    assert(event.wallTime instanceof Date);
    assertChangeStreamEventEq(event, expectedEvent);
}

function getCollectionUuid(coll) {
    const collInfo = testDB.getCollectionInfos({name: coll})[0];
    return collInfo.info.uuid;
}

function assertChangeEvent(operation, expectedEvent) {
    operation();
    assertNextChangeEvent(expectedEvent);
}

// Test change stream event for 'insert' operation.
assertChangeEvent(() => assert.commandWorked(coll.insert({_id: 0, a: 1})), {
    ns,
    operationType: 'insert',
    documentKey: {_id: 0},
    fullDocument: {_id: 0, a: 1},
    collectionUUID: getCollectionUuid(ns.coll),
});

// Test change stream event for replacement-style 'update' operation.
assertChangeEvent(() => assert.commandWorked(coll.update({_id: 0}, {_id: 0, a: 2})), {
    ns,
    operationType: 'replace',
    documentKey: {_id: 0},
    fullDocument: {_id: 0, a: 2},
    collectionUUID: getCollectionUuid(ns.coll),
});

// Test change stream event for modifier style 'update' operation.
assertChangeEvent(() => assert.commandWorked(coll.update({_id: 0}, {$inc: {a: 1}})), {
    ns,
    operationType: 'update',
    documentKey: {_id: 0},
    updateDescription:
        {removedFields: [], updatedFields: {a: 3}, truncatedArrays: [], disambiguatedPaths: {}},
    collectionUUID: getCollectionUuid(ns.coll),
});

// Test change stream event for modifier style 'update' operation with disambiguatedPaths.
// Set up initial value.
const update1 = {
    "arrayWithNumericField": [[{'0': "numeric", a: {'b.c': 1}, field: kLargeStr}]]
};
assertChangeEvent(() => assert.commandWorked(coll.update({_id: 0}, {$set: update1})), {
    ns,
    operationType: 'update',
    documentKey: {_id: 0},
    updateDescription:
        {removedFields: [], updatedFields: update1, truncatedArrays: [], disambiguatedPaths: {}},
    collectionUUID: getCollectionUuid(ns.coll),
});

// Update document so that 'disambiguatedPaths' will be populated.
const update2 = {
    "arrayWithNumericField.0.0.1": {"b.c": 'z'.repeat(30)}
};
assertChangeEvent(() => assert.commandWorked(coll.update({_id: 0}, {$set: update2})), {
    ns,
    operationType: 'update',
    documentKey: {_id: 0},
    updateDescription: {
        removedFields: [],
        updatedFields: update2,
        truncatedArrays: [],
        disambiguatedPaths: {"arrayWithNumericField.0.0.1": ["arrayWithNumericField", 0, 0, "1"]}
    },
    collectionUUID: getCollectionUuid(ns.coll),
});

// Test change stream event for 'remove' operation.
assertChangeEvent(() => assert.commandWorked(coll.remove({_id: 0})), {
    ns,
    operationType: 'delete',
    documentKey: {_id: 0},
    collectionUUID: getCollectionUuid(ns.coll),
});

// Test change stream event for 'rename' operation with 'dropTarget: false'.
assertChangeEvent(() => assert.commandWorked(coll.renameCollection(renamedCollName)), {
    ns,
    operationType: 'rename',
    to: renamedNs,
    operationDescription: {
        to: renamedNs,
    },
    collectionUUID: getCollectionUuid(ns.coll),
});

assertDropCollection(testDB, renamedCollName);
