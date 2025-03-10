/**
 * Tests bucket max size limit is respected with high cardinality workloads.
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: [
        {
            // Smaller cache size to make bucket max size calculation to drop under the limit
            // quickly.
            wiredTigerCacheSizeGB: 0.25,
            // To make sure that larger measurements can still be inserted into the same bucket.
            setParameter: {timeseriesBucketMinSize: 100000},
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const collName1 = `${jsTestName()}_1`;
const collName2 = `${jsTestName()}_2`;

assert.commandWorked(
    testDB.createCollection(collName1, {timeseries: {timeField: "time", metaField: "tag"}}));
assert.commandWorked(
    testDB.createCollection(collName2, {timeseries: {timeField: "time", metaField: "tag"}}));

// Inserts lots of buckets to coll1.
const coll1 = testDB.getCollection(collName1);
const bulk = coll1.initializeUnorderedBulkOp();
for (let i = 0; i < 100 * 1000; ++i) {
    bulk.insert({time: new Date(), tag: i});
}
assert.commandWorked(bulk.execute());

// Inserts to coll2 should still fit in the same buckets as long as the size doesn't exceed
// timeseriesBucketMinSize.
const coll2 = testDB.getCollection(collName2);
for (let i = 0; i < 50; ++i) {
    // Each measurement takes ~1KB.
    assert.commandWorked(coll2.insert({time: new Date(), tag: 0, strField: 'a'.repeat(1000)}));
}
assert.eq(testDB.getCollection(`system.buckets.${collName2}`).count(), 1);

rst.stopSet();