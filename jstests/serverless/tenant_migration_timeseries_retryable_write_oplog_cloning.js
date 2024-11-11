/**
 * Tests aggregation pipeline for cloning oplog chains for retryable writes on the tenant migration
 * donor that committed before a certain donor Timestamp for time-series collections oplog entries
 * with multiple statement IDs.
 *
 * This test is based on "tenant_migration_retryable_write_retry.js".
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   # We assume that all nodes in a mixed-mode replica set are using compressed inserts to
 *   # a time-series collection.
 *   requires_fcv_71,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {makeX509OptionsForTest} from "jstests/replsets/libs/tenant_migration_util.js";

function testOplogCloning(ordered) {
    const migrationX509Options = makeX509OptionsForTest();
    const kGarbageCollectionParams = {
        // Set the delay before a donor state doc is garbage collected to be short to speed up
        // the test.
        tenantMigrationGarbageCollectionDelayMS: 3 * 1000,

        // Set the TTL monitor to run at a smaller interval to speed up the test.
        ttlMonitorSleepSecs: 1,
    };

    const donorRst = new ReplSetTest({
        nodes: 1,
        name: "donor",
        serverless: true,
        nodeOptions:
            Object.assign(migrationX509Options.donor, {setParameter: kGarbageCollectionParams})
    });
    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: "recipient",
        serverless: true,
        nodeOptions:
            Object.assign(migrationX509Options.recipient, {setParameter: kGarbageCollectionParams})
    });

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});

    const donorPrimary = donorRst.getPrimary();

    const kTenantId = ObjectId().str;
    const kDbName = kTenantId + "_" +
        "tsDb";
    const kCollName = "tsColl";
    const kNs = `${kDbName}.${kCollName}`;

    const tsDB = donorPrimary.getDB(kDbName);
    assert.commandWorked(
        tsDB.createCollection(kCollName, {timeseries: {timeField: "time", metaField: "meta"}}));
    const tag1 = "regular insert";
    assert.commandWorked(donorPrimary.getCollection(kNs).insert(
        [
            {_id: 0, time: ISODate(), x: 0, tag: tag1, meta: 0},
            {_id: 1, time: ISODate(), x: 1, tag: tag1, meta: 0},
            {_id: 2, time: ISODate(), x: 2, tag: tag1, meta: 0},
        ],
        {writeConcern: {w: "majority"}}));

    const insertTag = "retryable insert";
    const updateTag = "retryable update";

    function verifyBuckets(buckets) {
        assert.eq(2, buckets.length);

        if (TimeseriesTest.timeseriesAlwaysUseCompressedBucketsEnabled(
                donorPrimary.getDB("admin"))) {
            TimeseriesTest.decompressBucket(buckets[0]);
            TimeseriesTest.decompressBucket(buckets[1]);
        }

        // First bucket checks
        assert.eq(0, buckets[0].control.min.x);
        assert.eq(5, buckets[0].control.max.x);
        assert.eq(0, buckets[0].meta);
        assert.eq(6, Object.keys(buckets[0].data.time).length);
        assert.eq(tag1, buckets[0].data.tag["0"]);
        assert.eq(tag1, buckets[0].data.tag["1"]);
        assert.eq(tag1, buckets[0].data.tag["2"]);
        assert.eq(updateTag, buckets[0].data.tag["3"]);
        assert.eq(updateTag, buckets[0].data.tag["4"]);
        assert.eq(updateTag, buckets[0].data.tag["5"]);

        // Second bucket checks
        assert.eq(0, buckets[1].control.min.x);
        assert.eq(2, buckets[1].control.max.x);
        assert.eq(1, buckets[1].meta);
        assert.eq(3, Object.keys(buckets[1].data.time).length);
        assert.eq(insertTag, buckets[1].data.tag["0"]);
        assert.eq(insertTag, buckets[1].data.tag["1"]);
        assert.eq(insertTag, buckets[1].data.tag["2"]);
    }

    jsTest.log("Run retryable writes prior to the migration");

    function runRetryableWrites(lsid) {
        assert.commandWorked(donorPrimary.getDB(kDbName).runCommand({
            insert: kCollName,
            documents: [
                // Test batched inserts resulting in "insert" oplog entries.
                {time: ISODate(), x: 0, tag: insertTag, meta: 1},
                {time: ISODate(), x: 1, tag: insertTag, meta: 1},
                {time: ISODate(), x: 2, tag: insertTag, meta: 1},
                // Test batched inserts resulting in "update" oplog entries.
                {time: ISODate(), x: 3, tag: updateTag, meta: 0},
                {time: ISODate(), x: 4, tag: updateTag, meta: 0},
                {time: ISODate(), x: 5, tag: updateTag, meta: 0},
            ],
            txnNumber: NumberLong(0),
            lsid: lsid,
            ordered: ordered,
        }));
    }

    const lsid = {id: UUID()};
    runRetryableWrites(lsid);

    jsTest.log("Run a migration to completion");
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, kTenantId);

    jsTest.log("Run retryable write again after the migration");
    runRetryableWrites(lsid);

    // Verify the contents of the bucket documents.
    verifyBuckets(tsDB.getCollection("system.buckets.tsColl").find().toArray());

    donorRst.stopSet();
    recipientRst.stopSet();
}

testOplogCloning(true);
testOplogCloning(false);
