/**
 * Tests that a time-series collection created with the 'expireAfterSeconds' option will remove
 * buckets older than 'expireAfterSeconds' based on the bucket creation time.
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_getmore,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";

const conn = MongoRunner.runMongod({setParameter: "ttlMonitorSleepSecs=1"});
const testDB = conn.getDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

TimeseriesTest.run((insert) => {
    const coll = testDB[jsTestName()];

    coll.drop();

    const timeFieldName = "time";
    const expireAfterSeconds = NumberLong(1);
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {
            timeseries: {timeField: timeFieldName},
            expireAfterSeconds: expireAfterSeconds,
        }),
    );

    // Inserts a measurement with a time in the past to ensure the measurement will be removed
    // immediately.
    const t = ISODate("2020-11-13T01:00:00Z");
    let start = ISODate();
    assert.lt(t, start);

    const doc = {_id: 0, [timeFieldName]: t, x: 0};
    assert.commandWorked(insert(coll, doc), "failed to insert doc: " + tojson(doc));
    jsTestLog("Insertion took " + (new Date().getTime() - start.getTime()) + " ms.");

    // Wait for the document to be removed.
    start = ISODate();
    assert.soon(() => {
        return 0 == coll.find().itcount();
    });
    jsTestLog("Removal took " + (new Date().getTime() - start.getTime()) + " ms.");

    // Check buckets.
    const bucketDocs = getTimeseriesCollForRawOps(testDB, coll).find().rawData().sort({"control.min._id": 1}).toArray();
    assert.eq(0, bucketDocs.length, bucketDocs);
});

MongoRunner.stopMongod(conn);
