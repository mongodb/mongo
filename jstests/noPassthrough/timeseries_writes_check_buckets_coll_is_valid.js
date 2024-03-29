/**
 * Tests that writes fail against a collection which appears to be time-series but the buckets
 * collection is missing required options.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();

const db = conn.getDB(jsTestName());
const coll = db.coll;
const bucketsColl = db.system.buckets.coll;

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

MongoRunner.stopMongod(conn);
})();
