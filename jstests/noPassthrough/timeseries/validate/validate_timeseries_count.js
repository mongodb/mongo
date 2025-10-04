/**
 * Tests that the validate command checks that the number of measurements in a time-series
 * collection matches the 'control.count' field.
 *
 * @tags: [
 * requires_fcv_62
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

let testCount = 0;
const collNamePrefix = jsTestName();
let collName = collNamePrefix + testCount;
let coll = null;

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

jsTestLog(
    "Running the validate command to check that time-series bucket 'control.count' matches the number of measurements in version-2 buckets.",
);
testCount += 1;
collName = collNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(
    db.createCollection(collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}),
);
coll = db.getCollection(collName);

// Inserts documents into a bucket. Checks no issues are found.
TimeseriesTest.insertManyDocs(coll);
let res = coll.validate();
assert(res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 0);
assert.eq(res.warnings.length, 0);

// Manually changes the control.count of a version-2 (compressed) bucket, expects warnings. The
// control.count field does not exist in version-1 buckets.
jsTestLog("Manually changing the 'control.count' of a version-2 bucket.");
testCount += 1;
collName = collNamePrefix + testCount;
db.getCollection(collName).drop();
assert.commandWorked(
    db.createCollection(collName, {timeseries: {timeField: "timestamp", metaField: "metadata", granularity: "hours"}}),
);
coll = db.getCollection(collName);
// Using insertMany means that these inserts will be performed in the same WriteBatch because the
// number of documents inserted is less than maxWriteBatchSize. This will insert two buckets due to
// the default timeseriesBucketMaxCount=1000 limit. We are always using compressed buckets to write
// to time-series collections, so this insert will be compressed (i.e. land in a version-2 bucket).
coll.insertMany(
    [...Array(1002).keys()].map((i) => ({
        "metadata": {"sensorId": 2, "type": "temperature"},
        "timestamp": ISODate(),
        "temp": i,
    })),
    {ordered: false},
);
getTimeseriesCollForRawOps(db, coll).updateOne(
    {"meta.sensorId": 2, "control.version": TimeseriesTest.BucketVersion.kCompressedSorted},
    {"$set": {"control.count": 10}},
    getRawOperationSpec(db),
);
res = coll.validate({checkBSONConformance: true});
assert(!res.valid, tojson(res));
assert.eq(res.nNonCompliantDocuments, 1);
assert.eq(res.errors.length, 1);

// As of SERVER-86451, time-series inconsistencies detected during validation
// will error in testing, instead of being warnings. In this case,
// validation on shutdown would fail, whereas before only a warning would be thrown.
MongoRunner.stopMongod(conn, null, {skipValidation: true});
