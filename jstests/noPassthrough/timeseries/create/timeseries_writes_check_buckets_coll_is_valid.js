/**
 * Tests that writes fail against a collection which appears to be time-series but the buckets
 * collection is missing required options.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const conn = MongoRunner.runMongod();

const db = conn.getDB(jsTestName());
const coll = db.coll;
const bucketsColl = db.system.buckets.coll;

configureFailPoint(db, "useRegularCreatePathForTimeseriesBucketsCreations");

const runTest = function(options) {
    assert.commandWorked(db.createCollection(bucketsColl.getName(), options));

    assert.commandFailedWithCode(coll.insert({t: ISODate(), m: 0, a: 1}),
                                 ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(coll.update({a: 1}, {$set: {a: 2}}), ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(coll.remove({a: 1}), ErrorCodes.InvalidOptions);

    assert(bucketsColl.drop());
};

// Not clustered
runTest({
    timeseries: {timeField: "t", metaField: "m", granularity: "seconds", bucketMaxSpanSeconds: 3600}
});
// Missing bucketMaxSpanSeconds
runTest(
    {clusteredIndex: true, timeseries: {timeField: "t", metaField: "m", granularity: "seconds"}});
// Missing granularity
runTest({
    clusteredIndex: true,
    timeseries: {timeField: "t", metaField: "m", bucketMaxSpanSeconds: 3600},
});

MongoRunner.stopMongod(conn);
