/**
 * Tests that running collStats against a time-series collection includes statistics specific to
 * time-series collections.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_getmore,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const kIdleBucketExpiryMemoryUsageThreshold = 1024 * 1024 * 10;
const conn = MongoRunner.runMongod({
    setParameter: {
        timeseriesIdleBucketExpiryMemoryUsageThreshold: kIdleBucketExpiryMemoryUsageThreshold,
        timeseriesBucketMinCount: 1,
    },
});

const dbName = jsTestName();
const testDB = conn.getDB(dbName);

assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection("t");

const timeFieldName = "time";
const metaFieldName = "m";

const expectedStats = {
    bucketsNs: getTimeseriesCollForDDLOps(testDB, coll).getFullName(),
};
let initialized = false;

const clearCollection = function () {
    coll.drop();
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    expectedStats.bucketCount = 0;
    expectedStats.numBucketInserts = 0;
    expectedStats.numBucketUpdates = 0;
    expectedStats.numBucketsOpenedDueToMetadata = 0;
    expectedStats.numBucketsClosedDueToCount = 0;
    expectedStats.numBucketsClosedDueToSize = 0;
    expectedStats.numBucketsClosedDueToTimeForward = 0;
    expectedStats.numBucketsClosedDueToMemoryThreshold = 0;
    expectedStats.numBucketsArchivedDueToMemoryThreshold = 0;
    expectedStats.numBucketsArchivedDueToTimeBackward = 0;
    expectedStats.numBucketsReopened = 0;
    expectedStats.numBucketsKeptOpenDueToLargeMeasurements = 0;
    expectedStats.numBucketsClosedDueToCachePressure = 0;
    expectedStats.numBucketsFetched = 0;
    expectedStats.numBucketsQueried = 0;
    expectedStats.numBucketFetchesFailed = 0;
    expectedStats.numBucketQueriesFailed = 0;
    TimeseriesTest.checkBucketReopeningsFailedCounters(expectedStats, {});
    expectedStats.numDuplicateBucketsReopened = 0;
    expectedStats.numCommits = 0;
    expectedStats.numWaits = 0;
    expectedStats.numMeasurementsCommitted = 0;
};
clearCollection();

const checkCollStats = function (empty = false, disableSizeClosureCheck = false) {
    const stats = assert.commandWorked(coll.stats());

    assert.eq(coll.getFullName(), stats.ns);

    for (let [stat, value] of Object.entries(expectedStats)) {
        if (stat === "numBucketsClosedDueToMemoryThreshold" || stat === "numBucketsArchivedDueToMemoryThreshold") {
            // Idle bucket expiration behavior will be non-deterministic since buckets are hashed
            // into shards within the catalog based on metadata, and expiration is done on a
            // per-shard basis. We just want to make sure that if we are expecting the number to be
            // sufficiently large under a global-expiry regime, that it is at least greater than 0,
            // signifying we have expired something from some shard.
            //
            // The value 33 was chosen as "sufficiently large" simply because we use 32 shards in
            // the BucketCatalog and so we can apply the pigeon-hole principle to conclude that at
            // least one of those inserted buckets that we expect to have triggered an expiration
            // did in fact land in a shard with an existing idle bucket that it could expire.
            if (value > 33) {
                assert.gte(
                    TimeseriesTest.getStat(stats.timeseries, stat),
                    1,
                    "Invalid 'timeseries." + stat + "' value in collStats: " + tojson(stats.timeseries),
                );
            }
        } else {
            if (disableSizeClosureCheck && stat == "numBucketsClosedDueToSize") {
                // With SERVER-107584 it is not possible to deterministically know when an archived
                // bucket will in fact be archived or closed. The current archival policy is set to
                // random, and a test cannot check size on random buckets.
                continue;
            } else {
                assert.eq(
                    TimeseriesTest.getStat(stats.timeseries, stat),
                    value,
                    "Invalid 'timeseries." + stat + "' value in collStats: " + tojson(stats.timeseries),
                );
            }
        }
    }

    if (empty) {
        assert(!stats.timeseries.hasOwnProperty("avgBucketSize"));
        assert(!stats.timeseries.hasOwnProperty("avgNumMeasurementsPerCommit"));
    } else {
        assert.gt(stats.timeseries.avgBucketSize, 0);
    }

    assert(!stats.timeseries.hasOwnProperty("count"));
    assert(!stats.timeseries.hasOwnProperty("avgObjSize"));

    if (expectedStats.numCompressedBuckets > 0) {
        assert.lt(
            stats.timeseries["numBytesCompressed"],
            stats.timeseries["numBytesUncompressed"],
            "Invalid 'timeseries.numBytesCompressed' value in collStats: " + tojson(stats.timeseries),
        );
    }
};
checkCollStats(true);

let docs = Array(3).fill({[timeFieldName]: ISODate(), [metaFieldName]: {a: 1}});
assert.commandWorked(coll.insert(docs, {ordered: false}));
expectedStats.bucketCount++;
expectedStats.numBucketInserts++;
expectedStats.numBucketsOpenedDueToMetadata++;
expectedStats.numCommits++;
expectedStats.numMeasurementsCommitted += 3;
expectedStats.avgNumMeasurementsPerCommit = 3;
expectedStats.numBucketQueriesFailed++;
checkCollStats();

assert.commandWorked(coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: {a: 2}}, {ordered: false}));
expectedStats.bucketCount++;
expectedStats.numBucketInserts++;
expectedStats.numBucketsOpenedDueToMetadata++;
expectedStats.numCommits++;
expectedStats.numMeasurementsCommitted++;
expectedStats.avgNumMeasurementsPerCommit = 2;
expectedStats.numBucketQueriesFailed++;
checkCollStats();

assert.commandWorked(coll.insert({[timeFieldName]: ISODate(), [metaFieldName]: {a: 2}}, {ordered: false}));
expectedStats.numBucketUpdates++;
expectedStats.numCommits++;
expectedStats.numMeasurementsCommitted++;
expectedStats.avgNumMeasurementsPerCommit = 1;
checkCollStats();

docs = Array(5).fill({[timeFieldName]: ISODate(), [metaFieldName]: {a: 2}});
assert.commandWorked(coll.insert(docs, {ordered: false}));
expectedStats.numBucketUpdates++;
expectedStats.numCommits++;
expectedStats.numMeasurementsCommitted += 5;
expectedStats.avgNumMeasurementsPerCommit = 2;
checkCollStats();

assert.commandWorked(
    coll.insert({[timeFieldName]: ISODate("2021-01-01T01:00:00Z"), [metaFieldName]: {a: 1}}, {ordered: false}),
);
expectedStats.bucketCount++;
expectedStats.numBucketInserts++;
expectedStats.numCommits++;
expectedStats.numBucketsArchivedDueToTimeBackward++;
expectedStats.numMeasurementsCommitted++;
expectedStats.numBucketQueriesFailed++;
checkCollStats();

// Assumes each bucket has a limit of 1000 measurements.
const bucketMaxCount = 1000;
let numDocs = bucketMaxCount + 100;
docs = Array(numDocs).fill({[timeFieldName]: ISODate(), [metaFieldName]: {a: "limit_count"}});
assert.commandWorked(coll.insert(docs, {ordered: false}));
expectedStats.bucketCount += 2;
expectedStats.numBucketInserts += 2;
expectedStats.numBucketsOpenedDueToMetadata++;
expectedStats.numBucketsClosedDueToCount++;
expectedStats.numCommits += 2;
expectedStats.numMeasurementsCommitted += numDocs;
expectedStats.avgNumMeasurementsPerCommit = Math.floor(
    expectedStats.numMeasurementsCommitted / expectedStats.numCommits,
);
expectedStats.numBucketQueriesFailed++;
checkCollStats();

// Assumes each bucket has a limit of 1000 measurements. We change the order twice of fields in the
// subobj we are storing. Should be 2 'numSubObjCompressionRestart' if bucket compression is
// enabled.
docs = Array(500).fill({[timeFieldName]: ISODate(), [metaFieldName]: {a: 37}, x: {"a": 1, "b": 1}});
docs = docs.concat(Array(1).fill({[timeFieldName]: ISODate(), [metaFieldName]: {a: 37}, x: {"b": 1, "a": 1}}));
docs = docs.concat(Array(500).fill({[timeFieldName]: ISODate(), [metaFieldName]: {a: 37}, x: {"a": 1, "b": 1}}));
assert.commandWorked(coll.insert(docs, {ordered: false}));
expectedStats.bucketCount += 2;
expectedStats.numBucketInserts += 2;
expectedStats.numBucketsOpenedDueToMetadata++;
expectedStats.numBucketsClosedDueToCount++;
expectedStats.numCommits += 2;
expectedStats.numMeasurementsCommitted += 1001;
expectedStats.avgNumMeasurementsPerCommit = Math.floor(
    expectedStats.numMeasurementsCommitted / expectedStats.numCommits,
);
expectedStats.numBucketQueriesFailed++;
checkCollStats();

// Assumes each bucket has a limit of 125kB on the measurements stored in the 'data' field.
const bucketMaxSizeKB = 125;
numDocs = 2;
// The measurement data should not take up all of the 'bucketMaxSizeKB' limit because we need
// to leave a little room for the _id and the time fields.
// This test leaves the bucket with a single measurement which will cause compression to be
// by-passed. The stats tracking of compressed buckets will thus also be by-passed.
let largeValue = "x".repeat((bucketMaxSizeKB - 1) * 1024);
docs = Array(numDocs).fill({[timeFieldName]: ISODate(), x: largeValue, [metaFieldName]: {a: "limit_size"}});
assert.commandWorked(coll.insert(docs, {ordered: false}));
expectedStats.bucketCount += numDocs;
expectedStats.numBucketInserts += numDocs;
expectedStats.numBucketsOpenedDueToMetadata++;
expectedStats.numBucketsClosedDueToSize++;
expectedStats.numCommits += numDocs;
expectedStats.numMeasurementsCommitted += numDocs;
expectedStats.avgNumMeasurementsPerCommit = Math.floor(
    expectedStats.numMeasurementsCommitted / expectedStats.numCommits,
);
expectedStats.numBucketQueriesFailed++;
checkCollStats();

// Assumes the measurements in each bucket span at most one hour (based on the time field).
// This test leaves just one measurement per bucket which will cause compression to be
// by-passed. The stats tracking of compressed buckets will thus also be by-passed.
const docTimes = [ISODate("2020-11-13T01:00:00Z"), ISODate("2020-11-13T03:00:00Z")];
numDocs = 2;
docs = [];
for (let i = 0; i < numDocs; i++) {
    docs.push({[timeFieldName]: docTimes[i], [metaFieldName]: {a: "limit_time_range"}});
}
assert.commandWorked(coll.insert(docs, {ordered: false}));
expectedStats.bucketCount += numDocs;
expectedStats.numBucketInserts += numDocs;
expectedStats.numBucketsOpenedDueToMetadata++;
expectedStats.numBucketsClosedDueToTimeForward++;
expectedStats.numCommits += numDocs;
expectedStats.numMeasurementsCommitted += numDocs;
expectedStats.avgNumMeasurementsPerCommit = Math.floor(
    expectedStats.numMeasurementsCommitted / expectedStats.numCommits,
);
expectedStats.numBucketQueriesFailed += 1;
checkCollStats();

numDocs = 70;
largeValue = "a".repeat(1024 * 1024);

const testIdleBucketExpiry = function (docFn) {
    clearCollection();

    let memoryUsage = 0;
    let shouldExpire = false;
    for (let i = 0; i < numDocs; i++) {
        assert.commandWorked(coll.insert(docFn(i), {ordered: false}));
        memoryUsage = assert.commandWorked(testDB.serverStatus()).bucketCatalog.memoryUsage;

        expectedStats.bucketCount++;
        expectedStats.numBucketInserts++;
        expectedStats.numBucketsOpenedDueToMetadata++;
        if (shouldExpire) {
            expectedStats.numBucketsArchivedDueToMemoryThreshold++;
        }
        expectedStats.numCommits++;
        expectedStats.numMeasurementsCommitted++;
        expectedStats.avgNumMeasurementsPerCommit = Math.floor(
            expectedStats.numMeasurementsCommitted / expectedStats.numCommits,
        );
        expectedStats.numBucketQueriesFailed++;
        checkCollStats(/*empty=*/ false, /*disableSizeClosureCheck=*/ true);

        shouldExpire = memoryUsage > kIdleBucketExpiryMemoryUsageThreshold;
    }

    assert(
        shouldExpire,
        `Memory usage did not reach idle bucket expiry threshold: ${memoryUsage} < ${
            kIdleBucketExpiryMemoryUsageThreshold
        }`,
    );
};

testIdleBucketExpiry((i) => {
    return {[timeFieldName]: ISODate(), [metaFieldName]: {[i.toString()]: largeValue}};
});
testIdleBucketExpiry((i) => {
    return {[timeFieldName]: ISODate(), [metaFieldName]: i, a: largeValue};
});

MongoRunner.stopMongod(conn);
