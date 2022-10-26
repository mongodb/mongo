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

load("jstests/core/timeseries/libs/timeseries.js");

const testDB = db.getSiblingDB(jsTestName());

const timeFieldName = 'time';
const metaFieldName = 'meta';

const collNamePrefix = 'timeseries_list_collections_';
let collCount = 0;

const bucketMaxSpanSecondsFromMinutes =
    TimeseriesTest.getBucketMaxSpanSecondsFromGranularity('minutes');
const buckeRoundingSecondsFromMinutes =
    TimeseriesTest.getBucketRoundingSecondsFromGranularity('minutes');

const testOptions = function(options) {
    const coll = db.getCollection(collNamePrefix + collCount++);
    coll.drop();

    jsTestLog('Creating time-series collection with options: ' + tojson(options));
    assert.commandWorked(db.createCollection(coll.getName(), options));

    if (!options.timeseries.hasOwnProperty('granularity')) {
        Object.assign(options.timeseries, {granularity: 'seconds'});
    }
    if (!options.timeseries.hasOwnProperty('bucketMaxSpanSeconds')) {
        Object.assign(options.timeseries, {
            bucketMaxSpanSeconds: TimeseriesTest.getBucketMaxSpanSecondsFromGranularity(
                options.timeseries.granularity)
        });
    }
    if (TimeseriesTest.timeseriesScalabilityImprovementsEnabled(testDB)) {
        // When we are using default 'granularity' values we won't actually set
        // 'bucketRoundingSeconds' internally.
        if (options.timeseries.hasOwnProperty('bucketRoundingSeconds')) {
            delete options.timeseries.bucketRoundingSeconds;
        }
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
if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(testDB)) {
    testOptions({
        timeseries: {
            timeField: timeFieldName,
            granularity: 'minutes',
            bucketMaxSpanSeconds: bucketMaxSpanSecondsFromMinutes,
        }
    });
} else {
    testOptions({
        timeseries: {
            timeField: timeFieldName,
            granularity: 'minutes',
            bucketMaxSpanSeconds: bucketMaxSpanSecondsFromMinutes,
            bucketRoundingSeconds: buckeRoundingSecondsFromMinutes,
        }
    });
}
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
if (!TimeseriesTest.timeseriesScalabilityImprovementsEnabled(testDB)) {
    testOptions({
        timeseries: {
            timeField: timeFieldName,
            metaField: metaFieldName,
            granularity: 'minutes',
            bucketMaxSpanSeconds: bucketMaxSpanSecondsFromMinutes,
        },
        storageEngine: {wiredTiger: {}},
        indexOptionDefaults: {storageEngine: {wiredTiger: {}}},
        collation: {locale: 'ja'},
        expireAfterSeconds: NumberLong(100),
    });
} else {
    testOptions({
        timeseries: {
            timeField: timeFieldName,
            metaField: metaFieldName,
            granularity: 'minutes',
            bucketMaxSpanSeconds: bucketMaxSpanSecondsFromMinutes,
            bucketRoundingSeconds: buckeRoundingSecondsFromMinutes,
        },
        storageEngine: {wiredTiger: {}},
        indexOptionDefaults: {storageEngine: {wiredTiger: {}}},
        collation: {locale: 'ja'},
        expireAfterSeconds: NumberLong(100),
    });
}
})();
