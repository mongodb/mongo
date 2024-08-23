/**
 * Tests that direct removal in a timeseries bucket collection close the relevant bucket, preventing
 * further inserts from landing in that bucket, including the case where a concurrent catalog write
 * causes a write conflict.
 *
 * TODO SERVER-93419: Investigate replacing this test's coverage with an FSM workload.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

const collName = 'test';

const timeFieldName = 'time';
const times = [
    ISODate('2021-01-01T01:00:00Z'),
    ISODate('2021-01-01T01:10:00Z'),
    ISODate('2021-01-01T01:20:00Z')
];
let docs = [
    {_id: 0, [timeFieldName]: times[0]},
    {_id: 1, [timeFieldName]: times[1]},
    {_id: 2, [timeFieldName]: times[2]}
];

const coll = testDB.getCollection(collName);
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());
coll.drop();

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

assert.commandWorked(coll.insert(docs[0]));
assert.docEq(docs.slice(0, 1), coll.find().sort({_id: 1}).toArray());

let buckets = bucketsColl.find().sort({_id: 1}).toArray();
assert.eq(buckets.length, 1);
assert.eq(buckets[0].control.min[timeFieldName], times[0]);
assert.eq(buckets[0].control.max[timeFieldName], times[0]);
const originalBucketId = bucketsColl.find({}).toArray()[0]._id;

const fpInsert = configureFailPoint(conn, "hangTimeseriesInsertBeforeWrite");
const awaitInsert = startParallelShell(
    funWithArgs(function(dbName, collName, doc) {
        assert.commandWorked(db.getSiblingDB(dbName).getCollection(collName).insert(doc));
    }, dbName, coll.getName(), docs[1]), conn.port);
fpInsert.wait();

const fpRemove = configureFailPoint(conn, "hangTimeseriesDirectModificationBeforeWriteConflict");
const awaitRemove = startParallelShell(
    funWithArgs(function(dbName, collName, id) {
        const removeResult = assert.commandWorked(
            db.getSiblingDB(dbName).getCollection('system.buckets.' + collName).remove({_id: id}));
        assert.eq(removeResult.nRemoved, 1);
    }, dbName, coll.getName(), buckets[0]._id), conn.port);
fpRemove.wait();

fpInsert.off();
fpRemove.off();
awaitRemove();
awaitInsert();

// The possible outcomes are that the inserts land in the same bucket and are deleted together, or
// that the delete happens before the insert and the insert lands in a new bucket. In the first
// case, there will be no documents in the collection, and in the second case, there will be a new
// bucket with a different id.
if (0 != coll.find().sort({_id: 1}).toArray().length) {
    assert.neq(originalBucketId, bucketsColl.find({}).toArray()[0]._id);
} else {
    buckets = bucketsColl.find().sort({_id: 1}).toArray();
    assert.eq(buckets.length, 0);

    // Now another insert should generate a new bucket.

    assert.commandWorked(coll.insert(docs[2]));
    assert.docEq(docs.slice(2, 3), coll.find().sort({_id: 1}).toArray());

    buckets = bucketsColl.find().sort({_id: 1}).toArray();
    assert.eq(buckets.length, 1);
    assert.eq(buckets[0].control.min[timeFieldName], times[2]);
    assert.eq(buckets[0].control.max[timeFieldName], times[2]);
}
MongoRunner.stopMongod(conn);
