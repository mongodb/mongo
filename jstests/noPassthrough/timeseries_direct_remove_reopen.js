/**
 * Tests that direct removal in a timeseries bucket collection synchronizes with bucket reopening.
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/feature_flag_util.js");  // For isEnabled.
load("jstests/libs/parallel_shell_helpers.js");

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

if (!FeatureFlagUtil.isEnabled(testDB, "TimeseriesScalabilityImprovements")) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesScalabilityImprovements feature flag is not enabled.");
    MongoRunner.stopMongod(conn);
    return;
}

const collName = 'test';

const metaFieldName = 'meta';
const timeFieldName = 'time';
const times = [ISODate('2021-01-01T01:00:00Z'), ISODate('2021-01-01T01:10:00Z')];
let docs = [
    {_id: 0, [timeFieldName]: times[0], [metaFieldName]: 1},
    {_id: 1, [timeFieldName]: times[1], [metaFieldName]: 1}
];

const coll = testDB.getCollection(collName);
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
coll.drop();

assert.commandWorked(testDB.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

assert.commandWorked(coll.insert(docs[0]));
assert.docEq(docs.slice(0, 1), coll.find().sort({_id: 1}).toArray());

let buckets = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(buckets.length, 1);
assert.eq(buckets[0].control.min[timeFieldName], times[0]);
assert.eq(buckets[0].control.max[timeFieldName], times[0]);
const oldId = buckets[0]._id;

// Start removing the bucket, but pause after the initial call to clear the bucket from the catalog,
// prior to actually removing the bucket from disk.
const fpClear = configureFailPoint(conn, "hangTimeseriesDirectModificationAfterStart");
const fpOnCommit = configureFailPoint(conn, "hangTimeseriesDirectModificationBeforeFinish");
const awaitRemove = startParallelShell(
    funWithArgs(function(dbName, collName, id) {
        const removeResult = assert.commandWorked(
            db.getSiblingDB(dbName).getCollection('system.buckets.' + collName).remove({_id: id}));
        assert.eq(removeResult.nRemoved, 1);
    }, dbName, coll.getName(), buckets[0]._id), conn.port);
fpClear.wait();

// Start inserting a bucket. We should find that there's no open bucket, since it's been cleared,
// but that there's an eligible bucket on disk. Pause before actually reopening it in the catalog.
const fpReopen = configureFailPoint(conn, "hangTimeseriesInsertBeforeReopeningBucket");
const awaitInsert = startParallelShell(
    funWithArgs(function(dbName, collName, doc) {
        assert.commandWorked(db.getSiblingDB(dbName).getCollection(collName).insert(doc));
    }, dbName, coll.getName(), docs[1]), conn.port);
fpReopen.wait();

// Now proceed to delete the bucket from disk, but pause before we clear it from the catalog again
// in the onCommit handler.
fpClear.off();
fpOnCommit.wait();

// Now let both operations proceed.
fpReopen.off();
fpOnCommit.off();
awaitRemove();
awaitInsert();

// The expected ordering is that the remove finishes, then the insert opens a new bucket.

assert.docEq(1, coll.find().sort({_id: 1}).toArray().length);

buckets = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(buckets.length, 1);
assert.neq(buckets[0]._id, oldId);

MongoRunner.stopMongod(conn);
})();
