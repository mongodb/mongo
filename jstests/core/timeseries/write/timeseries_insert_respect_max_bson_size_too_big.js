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
 *   # On slower variants, inserting the very large document creates an interruptible window long
 *   # enough that operations like taking the critical section during upgrades/downgrades or running
 *   # the balancer can consistently interrupt the operation enough for the test to not make progress.
 *   tsan_incompatible,
 *   incompatible_aubsan,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

// Retries the insert if it fails with InterruptedDueToReplStateChange (e.g. due to a stepdown),
// then asserts that the final result fails with the expected error code.
function assertInsertFailedWithCode(insertFn, expectedCode) {
    assert.soon(() => {
        const res = insertFn();
        if (res.code == ErrorCodes.InterruptedDueToReplStateChange) {
            jsTest.log.info("Retrying insert after transient command error: " + tojson(res));
            return false;
        }
        assert.commandFailedWithCode(res, expectedCode);
        return true;
    }, "insert did not fail with expected error code " + expectedCode);
}

let counter = 0;
TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    const coll = testDB["coll_" + counter++];
    const timestamp = ISODate("2025-01-01T12:00:00Z");
    const timeField = "t";
    const metaField = "m";

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {"timeField": timeField, "metaField": metaField}}),
    );

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
    assertInsertFailedWithCode(() => insert(coll, measurement1), ErrorCodes.BSONObjectTooLarge);

    // This measurement is always too big due to total metric size being copied into control block.
    assertInsertFailedWithCode(() => insert(coll, measurement2), ErrorCodes.BSONObjectTooLarge);
});
