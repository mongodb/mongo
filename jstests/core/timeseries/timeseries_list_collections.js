/**
 * Tests the result of running listCollections when there are time-series collections present.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
'use strict';

const timeFieldName = 'time';
const metaFieldName = 'meta';

const collNamePrefix = 'timeseries_list_collections_';
let collCount = 0;

const getBucketMaxSpanSeconds = function(granularity) {
    switch (granularity) {
        case 'seconds':
            return 60 * 60;
        case 'minutes':
            return 60 * 60 * 24;
        case 'hours':
            return 60 * 60 * 24 * 30;
        default:
            assert(false, 'Invalid granularity: ' + granularity);
    }
};

const testOptions = function(options) {
    const coll = db.getCollection(collNamePrefix + collCount++);
    coll.drop();

    jsTestLog('Creating time-series collection with options: ' + tojson(options));
    assert.commandWorked(db.createCollection(coll.getName(), options));

    if (!options.timeseries.hasOwnProperty('granularity')) {
        Object.assign(options.timeseries, {granularity: 'seconds'});
    }
    if (!options.timeseries.hasOwnProperty('bucketMaxSpanSeconds')) {
        Object.assign(
            options.timeseries,
            {bucketMaxSpanSeconds: getBucketMaxSpanSeconds(options.timeseries.granularity)});
    }

    if (options.hasOwnProperty('collation')) {
        Object.assign(options.collation, {
            caseLevel: false,
            caseFirst: 'off',
            strength: 3,
            numericOrdering: false,
            alternate: 'non-ignorable',
            maxVariable: 'punct',
            normalization: false,
            backwards: false,
            version: '57.1',
        });
    }

    const collections = assert.commandWorked(db.runCommand({listCollections: 1})).cursor.firstBatch;
    jsTestLog('Checking listCollections result: ' + tojson(collections));
    // Expected number of collections >= system.views + 2 * timeseries collections
    // 'test' database may contain collections from other tests running in parallel.
    assert.gte(collections.length, (collCount * 2 + 1));
    assert(collections.find(entry => entry.name === 'system.views'));
    assert(collections.find(entry => entry.name === 'system.buckets.' + coll.getName()));
    assert.docEq(
        collections.find(entry => entry.name === coll.getName()),
        {name: coll.getName(), type: 'timeseries', options: options, info: {readOnly: false}});
};

testOptions({timeseries: {timeField: timeFieldName}});
testOptions({timeseries: {timeField: timeFieldName, metaField: metaFieldName}});
testOptions({
    timeseries: {
        timeField: timeFieldName,
        granularity: 'minutes',
    }
});
testOptions({
    timeseries: {
        timeField: timeFieldName,
        granularity: 'minutes',
        bucketMaxSpanSeconds: 60 * 60 * 24,
    }
});
testOptions({
    timeseries: {
        timeField: timeFieldName,
    },
    storageEngine: {wiredTiger: {}},
});
testOptions({
    timeseries: {
        timeField: timeFieldName,
    },
    indexOptionDefaults: {storageEngine: {wiredTiger: {}}},
});
testOptions({
    timeseries: {
        timeField: timeFieldName,
    },
    collation: {locale: 'ja'},
});
testOptions({timeseries: {timeField: timeFieldName}, expireAfterSeconds: NumberLong(100)});
testOptions({
    timeseries: {
        timeField: timeFieldName,
        metaField: metaFieldName,
        granularity: 'minutes',
        bucketMaxSpanSeconds: 60 * 60 * 24,
    },
    storageEngine: {wiredTiger: {}},
    indexOptionDefaults: {storageEngine: {wiredTiger: {}}},
    collation: {locale: 'ja'},
    expireAfterSeconds: NumberLong(100),
});
})();
