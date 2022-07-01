/**
 * Tests index creation, index drops, list indexes, hide/unhide index on a time-series collection.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");
load("jstests/core/timeseries/libs/timeseries.js");

TimeseriesTest.run((insert) => {
    const collNamePrefix = 'timeseries_index_';
    let collCountPostfix = 0;

    const timeFieldName = 'tm';
    const metaFieldName = 'mm';
    const controlMinTimeFieldName = "control.min." + timeFieldName;
    const controlMaxTimeFieldName = "control.max." + timeFieldName;

    const doc = {
        _id: 0,
        [timeFieldName]: ISODate(),
        [metaFieldName]: {tag1: 'a', tag2: 'b', location: [1.0, 2.0]},
        loc: [0, 0]
    };

    const roundDown = (date) => {
        // Round down to nearest minute.
        return new Date(date - (date % 60000));
    };

    /**
     * Tests time-series
     *   - createIndex
     *   - queryable index (on time-series and underlying buckets collection using buckets format
     * hint)
     *   - dropIndex (by index name and key)
     *   - listIndexes
     *   - hide/unhide (index by index name and key)
     *   - createIndex w/ hidden:true
     *
     * Accepts two index key patterns.
     * The first key pattern is for the createIndexes command on the time-series collection.
     * The second key pattern is what we can expect to use as a hint when querying the bucket
     * collection.
     */
    const runTest = function(spec, bucketSpec) {
        const coll = db.getCollection(collNamePrefix + collCountPostfix++);
        const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
        coll.drop();  // implicitly drops bucketsColl.

        jsTestLog('Running test: collection: ' + coll.getFullName() +
                  ';\nindex spec key for createIndexes: ' + tojson(spec) +
                  ';\nindex spec for buckets collection: ' + tojson(bucketSpec));

        assert.commandWorked(db.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
        assert.contains(bucketsColl.getName(), db.getCollectionNames());

        // When the collection is sharded, there is 1 extra index for the shard key.
        const numExtraIndexes = (FixtureHelpers.isSharded(bucketsColl) ? 1 : 0) +
            (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db) ? 1 : 0);
        {
            const indexes = bucketsColl.getIndexes();
            assert.eq(numExtraIndexes,
                      indexes.length,
                      'unexpected number of indexes on the buckets collection: ' + tojson(indexes));
        }

        // Insert data on the time-series collection and index it.
        assert.commandWorked(insert(coll, doc), 'failed to insert doc: ' + tojson(doc));
        assert.commandWorked(coll.createIndex(spec), 'failed to create index: ' + tojson(spec));

        // Check that the buckets collection was created, the index on it is usable and the document
        // is present in the expected format.
        const bucketDocs = bucketsColl.find().hint(bucketSpec).toArray();
        assert.eq(1, bucketDocs.length, bucketDocs);

        const bucketDoc = bucketDocs[0];
        assert.eq(doc._id, bucketDoc.control.min._id, bucketDoc);
        assert.eq(roundDown(doc[timeFieldName]), bucketDoc.control.min[timeFieldName], bucketDoc);
        assert.docEq(doc[metaFieldName], bucketDoc.meta, bucketDoc);

        // Check that listIndexes against the time-series collection returns the index just created.
        //
        // Note: call the listIndexes command directly, rather than use a helper, so that we can
        // inspect the result's namespace in addition to the result's index key pattern.
        let cursorDoc = assert.commandWorked(db.runCommand({listIndexes: coll.getName()})).cursor;
        assert.eq(coll.getFullName(), cursorDoc.ns, tojson(cursorDoc));
        assert.eq(1 + numExtraIndexes, cursorDoc.firstBatch.length, tojson(cursorDoc));
        assert.contains(spec, cursorDoc.firstBatch.map(ix => ix.key), tojson(cursorDoc));

        // Check that listIndexes against the buckets collection returns the index as hinted
        cursorDoc =
            assert.commandWorked(db.runCommand({listIndexes: bucketsColl.getName()})).cursor;
        assert.eq(bucketsColl.getFullName(), cursorDoc.ns, tojson(cursorDoc));
        assert.eq(1 + numExtraIndexes, cursorDoc.firstBatch.length, tojson(cursorDoc));
        assert.contains(bucketSpec, cursorDoc.firstBatch.map(ix => ix.key), tojson(cursorDoc));

        // Drop the index on the time-series collection and then check that the underlying buckets
        // collection index was dropped properly.
        assert.commandWorked(coll.dropIndex(spec), 'failed to drop index: ' + tojson(spec));
        assert.commandFailedWithCode(
            assert.throws(() => bucketsColl.find().hint(bucketSpec).toArray()),
                         ErrorCodes.BadValue);
        assert.commandFailedWithCode(assert.throws(() => coll.find().hint(spec).toArray()),
                                                  ErrorCodes.BadValue);

        // Check that we are able to drop the index by name (single name and array of names).
        assert.commandWorked(coll.createIndex(spec, {name: 'myindex1'}),
                             'failed to create index: ' + tojson(spec));
        assert.commandWorked(coll.dropIndex('myindex1'), 'failed to drop index: myindex1');
        assert.commandWorked(coll.createIndex(spec, {name: 'myindex2'}),
                             'failed to create index: ' + tojson(spec));
        assert.commandWorked(coll.dropIndexes(['myindex2']), 'failed to drop indexes: [myindex2]');

        // Check that we are able to hide and unhide the index by name.
        assert.commandWorked(coll.createIndex(spec, {name: 'hide1'}),
                             'failed to create index: ' + tojson(spec));
        assert.eq(1, bucketsColl.find().hint(bucketSpec).toArray().length);
        assert.eq(1, coll.find().hint(spec).toArray().length);
        assert.commandWorked(coll.hideIndex('hide1'), 'failed to hide index: hide1');
        assert.commandFailedWithCode(
            assert.throws(() => bucketsColl.find().hint(bucketSpec).toArray()),
                         ErrorCodes.BadValue);
        assert.commandFailedWithCode(assert.throws(() => coll.find().hint(spec).toArray()),
                                                  ErrorCodes.BadValue);
        assert.commandWorked(coll.unhideIndex('hide1'), 'failed to unhide index: hide1');
        assert.eq(1, bucketsColl.find().hint(bucketSpec).toArray().length);
        assert.eq(1, coll.find().hint(spec).toArray().length);
        assert.commandWorked(coll.dropIndex('hide1'), 'failed to drop index: hide1');

        // Check that we are able to hide and unhide the index by key.
        assert.commandWorked(coll.createIndex(spec, {name: 'hide2'}),
                             'failed to create index: ' + tojson(spec));
        assert.eq(1, bucketsColl.find().hint(bucketSpec).toArray().length);
        assert.eq(1, coll.find().hint(spec).toArray().length);
        assert.commandWorked(coll.hideIndex(spec), 'failed to hide index: hide2');
        assert.commandFailedWithCode(
            assert.throws(() => bucketsColl.find().hint(bucketSpec).toArray()),
                         ErrorCodes.BadValue);
        assert.commandFailedWithCode(assert.throws(() => coll.find().hint(spec).toArray()),
                                                  ErrorCodes.BadValue);
        assert.commandWorked(coll.unhideIndex(spec), 'failed to unhide index: hide2');
        assert.eq(1, bucketsColl.find().hint(bucketSpec).toArray().length);
        assert.eq(1, coll.find().hint(spec).toArray().length);
        assert.commandWorked(coll.dropIndex('hide2'), 'failed to drop index: hide2');

        // Check that we are able to create the index as hidden.
        assert.commandWorked(coll.createIndex(spec, {name: 'hide3', hidden: true}),
                             'failed to create index: ' + tojson(spec));
        assert.commandFailedWithCode(
            assert.throws(() => bucketsColl.find().hint(bucketSpec).toArray()),
                         ErrorCodes.BadValue);
        assert.commandFailedWithCode(assert.throws(() => coll.find().hint(spec).toArray()),
                                                  ErrorCodes.BadValue);
        assert.commandWorked(coll.unhideIndex(spec), 'failed to unhide index: hide3');
        assert.eq(1, bucketsColl.find().hint(bucketSpec).toArray().length);
        assert.eq(1, coll.find().hint(spec).toArray().length);
        assert.commandWorked(coll.dropIndex('hide3'), 'failed to drop index: hide3');

        // Check that user hints on queries will be allowed and will reference the indexes on the
        // buckets collection directly.
        assert.commandWorked(coll.createIndex(spec, {name: 'index_for_hint_test'}),
                             'failed to create index index_for_hint_test: ' + tojson(spec));
        // Specifying the index by name should work on both the time-series collection and the
        // underlying buckets collection.
        assert.eq(1, bucketsColl.find().hint('index_for_hint_test').toArray().length);
        assert.eq(1, coll.find().hint('index_for_hint_test').toArray().length);
        // Specifying the index by key pattern should work.
        assert.eq(1, bucketsColl.find().hint(bucketSpec).toArray().length);
        assert.eq(1, coll.find().hint(spec).toArray().length);
        assert.commandWorked(coll.dropIndex('index_for_hint_test'),
                             'failed to drop index: index_for_hint_test');
    };

    /**
     * Time-series index creation and usage testing.
     */

    // metaField ascending and descending indexes.
    runTest({[metaFieldName]: 1}, {meta: 1});
    runTest({[metaFieldName]: -1}, {meta: -1});

    // metaField subfield indexes.
    runTest({[metaFieldName + '.tag1']: 1}, {'meta.tag1': 1});
    runTest({[metaFieldName + '.tag1']: 1, [metaFieldName + '.tag2']: -1},
            {'meta.tag1': 1, 'meta.tag2': -1});

    // timeField ascending and descending indexes.
    runTest({[timeFieldName]: 1}, {[controlMinTimeFieldName]: 1, [controlMaxTimeFieldName]: 1});
    runTest({[timeFieldName]: -1}, {[controlMaxTimeFieldName]: -1, [controlMinTimeFieldName]: -1});

    // Compound metaField and timeField.
    runTest({[metaFieldName + '.tag1']: 1, [timeFieldName]: 1},
            {'meta.tag1': 1, [controlMinTimeFieldName]: 1, [controlMaxTimeFieldName]: 1});
    runTest({[metaFieldName + '.tag1']: 1, [timeFieldName]: -1},
            {'meta.tag1': 1, [controlMaxTimeFieldName]: -1, [controlMinTimeFieldName]: -1});

    // Multi-metaField sub-fields and timeField compound index.
    runTest({[metaFieldName + '.tag1']: -1, [metaFieldName + '.tag2']: 1, [timeFieldName]: 1}, {
        'meta.tag1': -1,
        'meta.tag2': 1,
        [controlMinTimeFieldName]: 1,
        [controlMaxTimeFieldName]: 1
    });

    // metaField hashed index.
    runTest({[metaFieldName]: "hashed"}, {'meta': "hashed"});

    // metaField geo-type indexes.
    runTest({[metaFieldName + '.location']: "2dsphere"}, {'meta.location': "2dsphere"});
    runTest({[metaFieldName + '.location']: "2d"}, {'meta.location': "2d"});

    // compound geo-type indexes on metaField
    runTest({[metaFieldName + '.location']: "2dsphere", [metaFieldName + '.tag1']: -1},
            {'meta.location': "2dsphere", 'meta.tag1': -1});
    runTest({[metaFieldName + '.tag1']: -1, [metaFieldName + '.location']: "2dsphere"},
            {'meta.tag1': -1, 'meta.location': "2dsphere"});
    runTest({[metaFieldName + '.location']: "2d", [metaFieldName + '.tag1']: -1},
            {'meta.location': "2d", 'meta.tag1': -1});

    if (TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
        // Measurement 2dsphere index
        runTest({'loc': '2dsphere'}, {'data.loc': '2dsphere_bucket'});
    }

    /*
     * Test time-series index creation error handling.
     */

    const coll = db.getCollection(collNamePrefix + collCountPostfix++);
    coll.drop();
    jsTestLog('Checking index creation error handling on collection: ' + coll.getFullName());

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    assert.commandWorked(insert(coll, doc), 'failed to insert doc: ' + tojson(doc));

    if (!TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
        // Reject index keys that do not include the metadata field.
        assert.commandFailedWithCode(coll.createIndex({not_metadata: 1}),
                                     ErrorCodes.CannotCreateIndex);
        assert.commandFailedWithCode(coll.hideIndex({not_metadata: 1}), ErrorCodes.IndexNotFound);
        assert.commandFailedWithCode(coll.dropIndex({not_metadata: 1}), ErrorCodes.IndexNotFound);
    } else {
        assert.commandWorked(coll.createIndex({not_metadata: 1}));
        assert.commandWorked(coll.hideIndex({not_metadata: 1}));
        assert.commandWorked(coll.dropIndex({not_metadata: 1}));
    }

    // Index names are not transformed. dropIndexes passes the request along to the buckets
    // collection, which in this case does not possess the index by that name.
    assert.commandFailedWithCode(coll.dropIndex('mm_1'), ErrorCodes.IndexNotFound);

    const testCreateIndexFailed = function(spec, options = {}) {
        const indexName = 'testCreateIndex';
        const res = coll.createIndex(spec, Object.extend({name: indexName}, options));
        assert.commandFailedWithCode(res,
                                     [ErrorCodes.CannotCreateIndex, ErrorCodes.InvalidOptions]);
    };

    if (!TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
        // Partial indexes are not supported on time-series collections if the time-series metric
        // feature flag is disabled.
        testCreateIndexFailed({[metaFieldName]: 1}, {partialFilterExpression: {meta: {$gt: 5}}});
        testCreateIndexFailed({[metaFieldName]: 1},
                              {partialFilterExpression: {[metaFieldName]: {$gt: 5}}});
    }

    // Unique indexes are not supported on clustered collections.
    testCreateIndexFailed({[metaFieldName]: 1}, {unique: true});

    // TTL indexes are not supported on time-series collections.
    testCreateIndexFailed({[metaFieldName]: 1}, {expireAfterSeconds: 3600});

    // Text indexes are not supported on time-series collections.
    testCreateIndexFailed({[metaFieldName]: 'text'});

    // If listIndexes fails to convert a non-conforming index on the bucket collection, it should
    // omit that index from the results.
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    assert.commandWorked(bucketsColl.createIndex({not_metadata: 1}),
                         'failed to create index: ' + tojson({not_metadata: 1}));
    const numExtraIndexes = (FixtureHelpers.isSharded(bucketsColl) ? 1 : 0) +
        (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(db) ? 1 : 0);
    assert.eq(
        1 + numExtraIndexes, bucketsColl.getIndexes().length, tojson(bucketsColl.getIndexes()));
    assert.eq(0 + numExtraIndexes, coll.getIndexes().length, tojson(coll.getIndexes()));

    // Cannot directly create a "2dsphere_bucket" index.
    testCreateIndexFailed({"loc": "2dsphere_bucket"});

    // {} is not a valid index spec.
    testCreateIndexFailed({});

    // Hints are not valid index specs.
    testCreateIndexFailed({$natural: 1});
    testCreateIndexFailed({$natural: -1});
    testCreateIndexFailed({$hint: 'my_index_name'});
});
})();
