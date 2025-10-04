/**
 * Tests that direct updates to a timeseries bucket close the bucket, preventing further
 * inserts to land in that bucket or deletes and updates to be applied to it.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

const collName = "test";

const timeFieldName = "time";
const metaFieldName = "tag";
const times = [ISODate("2021-01-01T01:00:00Z"), ISODate("2021-01-01T01:10:00Z"), ISODate("2021-01-01T01:20:00Z")];
let docs = [
    {_id: 0, [timeFieldName]: times[0], [metaFieldName]: "A", f: 0},
    {_id: 1, [timeFieldName]: times[1], [metaFieldName]: "B", f: 1},
    {_id: 2, [timeFieldName]: times[2], [metaFieldName]: "C", f: 2},
];

const coll = testDB.getCollection(collName);
coll.drop();

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
);

assert.commandWorked(coll.insert(docs[0]));
assert.docEq(docs.slice(0, 1), coll.find().sort({_id: 1}).toArray());

let buckets = getTimeseriesCollForRawOps(testDB, coll).find().rawData().sort({_id: 1}).toArray();
assert.eq(buckets.length, 1);
assert.eq(buckets[0].control.min[timeFieldName], times[0]);
assert.eq(buckets[0].control.max[timeFieldName], times[0]);

let modified = buckets[0];
modified.control.closed = true;
let updateResult = assert.commandWorked(
    getTimeseriesCollForRawOps(testDB, coll).update({_id: buckets[0]._id}, modified, getRawOperationSpec(testDB)),
);
assert.eq(updateResult.nMatched, 1);
assert.eq(updateResult.nModified, 1);

buckets = getTimeseriesCollForRawOps(testDB, coll).find().rawData().sort({_id: 1}).toArray();
assert.eq(buckets.length, 1);
assert.eq(buckets[0].control.min[timeFieldName], times[0]);
assert.eq(buckets[0].control.max[timeFieldName], times[0]);
assert(buckets[0].control.closed);

assert.commandWorked(coll.insert(docs[1]));
assert.docEq(docs.slice(0, 2), coll.find().sort({_id: 1}).toArray());

buckets = getTimeseriesCollForRawOps(testDB, coll).find().rawData().sort({_id: 1}).toArray();
assert.eq(buckets.length, 2);
assert.eq(buckets[1].control.min[timeFieldName], times[1]);
assert.eq(buckets[1].control.max[timeFieldName], times[1]);

let fpInsert = configureFailPoint(conn, "hangTimeseriesInsertBeforeCommit");
let awaitInsert = startParallelShell(
    funWithArgs(
        function (dbName, collName, doc) {
            assert.commandWorked(db.getSiblingDB(dbName).getCollection(collName).insert(doc));
        },
        dbName,
        coll.getName(),
        docs[2],
    ),
    conn.port,
);

fpInsert.wait();

modified = buckets[1];
modified.control.closed = true;
updateResult = assert.commandWorked(
    getTimeseriesCollForRawOps(testDB, coll).update({_id: buckets[1]._id}, modified, getRawOperationSpec(testDB)),
);
assert.eq(updateResult.nMatched, 1);
assert.eq(updateResult.nModified, 1);

fpInsert.off();
awaitInsert();

assert.docEq(docs.slice(0, 3), coll.find().sort({_id: 1}).toArray());

buckets = getTimeseriesCollForRawOps(testDB, coll).find().rawData().sort({_id: 1}).toArray();
assert.eq(buckets.length, 3);
assert.eq(buckets[1].control.min[timeFieldName], times[1]);
assert.eq(buckets[1].control.max[timeFieldName], times[1]);
assert(buckets[1].control.closed);
assert.eq(buckets[2].control.min[timeFieldName], times[2]);
assert.eq(buckets[2].control.max[timeFieldName], times[2]);
assert(!buckets[2].control.hasOwnProperty("closed"));

// Make sure that closed buckets are skipped by updates and deletes.
if (FeatureFlagUtil.isPresentAndEnabled(testDB, "TimeseriesUpdatesSupport")) {
    // The first two buckets containing documents 0 and 1 are closed, so we can only update the
    // third document in the last bucket.
    const result = assert.commandWorked(coll.updateMany({}, {$set: {newField: 123}}));
    assert.eq(result.matchedCount, 1, result);
    assert.eq(result.modifiedCount, 1, result);
    assert.docEq(
        docs.slice(2, 3),
        coll.find({newField: 123}, {newField: 0}).toArray(),
        `Expected exactly one document to be updated. ${coll.find().toArray()}`,
    );
}

if (FeatureFlagUtil.isPresentAndEnabled(testDB, "TimeseriesDeletesSupport")) {
    // The first two buckets containing documents 0 and 1 are closed, so we can only delete the
    // third document from the last bucket. Use a filter on 'f' so this is treated as a non-batched
    // multi delete.
    let result = assert.commandWorked(coll.deleteMany({f: {$in: [0, 1, 2]}}));
    assert.eq(result.deletedCount, 1);
    // Now use a filter on only the meta field so that we will use the batched timeseries delete
    // path.
    result = assert.commandWorked(coll.deleteMany({[metaFieldName]: "A"}));
    assert.eq(result.deletedCount, 0);
    // A completely empty filter should also skip closed buckets.
    result = assert.commandWorked(coll.deleteMany({}));
    assert.eq(result.deletedCount, 0);
    assert.docEq(
        docs.slice(0, 2),
        coll.find().sort({_id: 1}).toArray(),
        `Expected exactly one document to be deleted. ${coll.find().toArray()}`,
    );
}
MongoRunner.stopMongod(conn);
