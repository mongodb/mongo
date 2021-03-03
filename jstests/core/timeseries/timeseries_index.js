/**
 * Tests basic index creation and drops on a time-series collection.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 *     sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const collNamePrefix = 'timeseries_index_';
let collCountPostfix = 0;

const timeFieldName = 'tm';
const metaFieldName = 'mm';

const doc = {
    _id: 0,
    [timeFieldName]: ISODate(),
    [metaFieldName]: {tag1: 'a', tag2: 'b'}
};

/**
 * Accepts two index key patterns.
 * The first key pattern is for the createIndexes command on the time-series collection.
 * The second key pattern is what we can expect to use as a hint when querying the bucket
 * collection.
 */
const runTest = function(keyForCreate, hint) {
    const coll = db.getCollection(collNamePrefix + collCountPostfix++);
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();  // implicitly drops bucketsColl.

    jsTestLog('Running test: collection: ' + coll.getFullName() +
              '; index spec key for createIndexes: ' + tojson(keyForCreate) +
              '; index spec key for hint: ' + tojson(hint));

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    assert.commandWorked(coll.insert(doc, {ordered: false}),
                         'failed to insert doc: ' + tojson(doc));

    assert.commandWorked(coll.createIndex(keyForCreate),
                         'failed to create index: ' + tojson(keyForCreate));

    // Check bucket collection.
    const bucketDocs = bucketsColl.find().hint(hint).toArray();
    assert.eq(1, bucketDocs.length, bucketDocs);

    const bucketDoc = bucketDocs[0];
    assert.eq(doc._id, bucketDoc.control.min._id, bucketDoc);
    assert.eq(doc[timeFieldName], bucketDoc.control.min[timeFieldName], bucketDoc);
    assert.docEq(doc[metaFieldName], bucketDoc.meta, bucketDoc);

    // Check that the underlying buckets collection index was dropped properly.
    assert.commandWorked(coll.dropIndex(keyForCreate),
                         'failed to drop index: ' + tojson(keyForCreate));
    assert.commandFailedWithCode(assert.throws(() => bucketsColl.find().hint(hint).toArray()),
                                              ErrorCodes.BadValue);

    // Check that we are able to drop the index by name (single name and array of names).
    assert.commandWorked(coll.createIndex(keyForCreate, {name: 'myindex1'}),
                         'failed to create index: ' + tojson(keyForCreate));
    assert.commandWorked(coll.dropIndex('myindex1'), 'failed to drop index: myindex1');
    assert.commandWorked(coll.createIndex(keyForCreate, {name: 'myindex2'}),
                         'failed to create index: ' + tojson(keyForCreate));
    assert.commandWorked(coll.dropIndexes(['myindex2']), 'failed to drop indexes: [myindex2]');
};

runTest({[metaFieldName]: 1}, {meta: 1});
runTest({[metaFieldName]: -1}, {meta: -1});
runTest({[metaFieldName + '.tag1']: 1}, {'meta.tag1': 1});
runTest({[metaFieldName + '.tag1']: 1, [metaFieldName + '.tag2']: -1},
        {'meta.tag1': 1, 'meta.tag2': -1});

runTest({[timeFieldName]: 1},
        {['control.min.' + timeFieldName]: 1, ['control.max.' + timeFieldName]: 1});
runTest({[metaFieldName + '.tag1']: 1, [timeFieldName]: 1},
        {'meta.tag1': 1, ['control.min.' + timeFieldName]: 1, ['control.max.' + timeFieldName]: 1});
runTest({[timeFieldName]: -1},
        {['control.max.' + timeFieldName]: -1, ['control.min.' + timeFieldName]: -1});
runTest(
    {[metaFieldName + '.tag1']: 1, [timeFieldName]: -1},
    {'meta.tag1': 1, ['control.max.' + timeFieldName]: -1, ['control.min.' + timeFieldName]: -1});
runTest({[metaFieldName + '.tag1']: -1, [metaFieldName + '.tag2']: 1, [timeFieldName]: 1}, {
    'meta.tag1': -1,
    'meta.tag2': 1,
    ['control.min.' + timeFieldName]: 1,
    ['control.max.' + timeFieldName]: 1
});

// Check index creation error handling.

const coll = db.getCollection(collNamePrefix + collCountPostfix++);
coll.drop();
jsTestLog('Checking index creation error handling on collection: ' + coll.getFullName());

assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
assert.commandWorked(coll.insert(doc, {ordered: false}), 'failed to insert doc: ' + tojson(doc));

// Reject index keys that do not include the metadata field.
assert.commandFailedWithCode(coll.createIndex({not_metadata: 1}), ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(coll.dropIndex({not_metadata: 1}), ErrorCodes.IndexNotFound);

// Index names are not transformed. dropIndexes passes the request along to the buckets collection,
// which in this case does not possess the index by that name.
assert.commandFailedWithCode(coll.dropIndex('mm_1'), ErrorCodes.IndexNotFound);

// Unique indexes are not supported
assert.commandFailedWithCode(coll.createIndex({[metaFieldName]: 1}, {unique: true}),
                             ErrorCodes.InvalidOptions);
})();
