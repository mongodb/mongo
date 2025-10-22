/**
 * Tests that time-series inserts do not cause any underlying bucket documents to exceed the max
 * user BSON size.
 *
 * Bucket Insert: Measurements that are uninsertable due to exceeding the BSON size limit when a
 * bucket insert is generated to accommodate one measurement.
 *
 * @tags: [
 *   requires_timeseries,
 *   multiversion_incompatible,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

let counter = 0;
TimeseriesTest.run(insert => {
    const testDB = db.getSiblingDB(jsTestName());
    const coll = testDB["coll_" + counter++];
    const timestamp = ISODate("2025-01-01T12:00:00Z");
    const timeField = "t";
    const metaField = "m";

    coll.drop();

    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {"timeField": timeField, "metaField": metaField}}));

    const largeMeta = "a".repeat(16 * 1024 * 1024 + 1);
    const measurement1 = {};
    measurement1[timeField] = timestamp;
    measurement1[metaField] = largeMeta;
    measurement1["a"] = 1;

    const smallMeta = "5";
    const bigStr = "a".repeat(8000);
    const measurement2 = {};
    for (let i = 0; i < 1000; ++i) {
        measurement2[i.toString()] = bigStr;
    }
    measurement2[timeField] = timestamp;
    measurement2[metaField] = smallMeta;

    // Insert Measurements

    // This measurement is always too big due to meta.
    assert.commandFailedWithCode(insert(coll, measurement1), ErrorCodes.BSONObjectTooLarge);

    // This measurement is always too big due to total metric size being copied into control block.
    assert.commandFailedWithCode(insert(coll, measurement2), ErrorCodes.BSONObjectTooLarge);
});
})();
