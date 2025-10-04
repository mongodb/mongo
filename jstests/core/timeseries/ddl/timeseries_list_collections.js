/**
 * Tests the result of running listCollections when there are time-series collections present.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {
    areViewlessTimeseriesEnabled,
    getTimeseriesBucketsColl,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const testDB = db.getSiblingDB(jsTestName());

const timeFieldName = "time";
const metaFieldName = "m";

const collNamePrefix = jsTestName() + "_";
let collCount = 0;

const bucketMaxSpanSecondsFromMinutes = TimeseriesTest.getBucketMaxSpanSecondsFromGranularity("minutes");

const testOptions = function (options) {
    const coll = testDB.getCollection(collNamePrefix + collCount++);
    coll.drop();

    jsTestLog("Creating time-series collection with options: " + tojson(options));
    assert.commandWorked(testDB.createCollection(coll.getName(), options));

    if (!options.timeseries.hasOwnProperty("granularity")) {
        Object.assign(options.timeseries, {granularity: "seconds"});
    }
    if (!options.timeseries.hasOwnProperty("bucketMaxSpanSeconds")) {
        Object.assign(options.timeseries, {
            bucketMaxSpanSeconds: TimeseriesTest.getBucketMaxSpanSecondsFromGranularity(options.timeseries.granularity),
        });
    }
    // When we are using default 'granularity' values we won't actually set
    // 'bucketRoundingSeconds' internally.
    if (options.timeseries.hasOwnProperty("bucketRoundingSeconds")) {
        delete options.timeseries.bucketRoundingSeconds;
    }

    if (options.hasOwnProperty("collation")) {
        Object.assign(options.collation, {
            caseLevel: false,
            caseFirst: "off",
            strength: 3,
            numericOrdering: false,
            alternate: "non-ignorable",
            maxVariable: "punct",
            normalization: false,
            backwards: false,
            version: "57.1",
        });
    }

    const collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
    jsTestLog("Checking listCollections result: " + tojson(collections));
    if (areViewlessTimeseriesEnabled(db)) {
        // Expected number of collections >= timeseries collections
        // 'test' database may contain collections from other tests running in parallel.
        assert.gte(collections.length, collCount);
    } else {
        // Expected number of collections >= system.views + 2 * timeseries collections
        // 'test' database may contain collections from other tests running in parallel.
        assert.gte(collections.length, collCount * 2 + 1);
        assert(collections.find((entry) => entry.name === "system.views"));
    }

    const bucketsCollName = getTimeseriesBucketsColl(coll).getName();
    assert.eq(
        !areViewlessTimeseriesEnabled(db),
        collections.some((entry) => entry.name === bucketsCollName),
    );

    const collectionDocument = collections.find((entry) => entry.name === coll.getName());

    // Exclude the collection UUID from the comparison, as it is randomly generated.
    assert.eq(areViewlessTimeseriesEnabled(db), collectionDocument.info.uuid !== undefined);
    delete collectionDocument.info.uuid;

    assert.docEq(
        {name: coll.getName(), type: "timeseries", options: options, info: {readOnly: false}},
        collectionDocument,
    );
};

testOptions({timeseries: {timeField: timeFieldName}});
testOptions({timeseries: {timeField: timeFieldName, metaField: metaFieldName}});
testOptions({
    timeseries: {
        timeField: timeFieldName,
        granularity: "minutes",
    },
});
testOptions({
    timeseries: {
        timeField: timeFieldName,
        granularity: "minutes",
        bucketMaxSpanSeconds: bucketMaxSpanSecondsFromMinutes,
    },
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
    collation: {locale: "ja"},
});
testOptions({timeseries: {timeField: timeFieldName}, expireAfterSeconds: NumberLong(100)});
testOptions({
    timeseries: {
        timeField: timeFieldName,
        metaField: metaFieldName,
        granularity: "minutes",
        bucketMaxSpanSeconds: bucketMaxSpanSecondsFromMinutes,
    },
    storageEngine: {wiredTiger: {}},
    indexOptionDefaults: {storageEngine: {wiredTiger: {}}},
    collation: {locale: "ja"},
    expireAfterSeconds: NumberLong(100),
});
