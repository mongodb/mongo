/**
 * Verifies that a user rawData update can change control.min.<timeField> on a time-series bucket
 * without tripping the debug invariants on the internal bucket-write paths
 * (updateTimeseriesDocument / performAtomicTimeseriesWrites). Those invariants guard the
 * sharding-unaware internal write paths so that an internal re-format of a bucket can never change
 * control.min.time -- the shard key for collections sharded on time -- and thereby orphan the
 * bucket. The rawData API instead goes through collection_internal::updateDocument directly, so the
 * update must succeed. Because the internal write paths are sharding-unaware, a single replica set
 * is sufficient to exercise them; no sharded cluster is required.
 *
 * The node runs with timeseriesLessStrictBucketValidator=true so that validateBucketIdTimestamp
 * skips the _id <-> control.min.t equality check; otherwise strict bucket validation would reject
 * the rawData write (which deliberately makes control.min.t inconsistent with the bucket _id) with
 * DocumentValidationFailure before it reaches the update path we want to exercise.
 *
 * @tags: [
 *   requires_replication,
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   multiversion_incompatible,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("rawData update can change control.min.t on a time-series bucket", function () {
    before(() => {
        this.rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: {timeseriesLessStrictBucketValidator: true}},
        });
        this.rst.startSet();
        this.rst.initiate();
    });

    after(() => {
        this.rst.stopSet();
    });

    it("succeeds and does not trip the internal control.min.time-change invariant", () => {
        const testDB = this.rst.getPrimary().getDB(jsTestName());
        const coll = testDB["ts"];

        assert.commandWorked(
            testDB.createCollection(coll.getName(), {
                timeseries: {timeField: "t", metaField: "m", granularity: "seconds"},
            }),
        );

        const t0 = ISODate("2024-01-01T00:05:10Z");
        const t1 = ISODate("2024-01-01T00:05:20Z");
        assert.commandWorked(
            coll.insertMany([
                {_id: 0, t: t0, m: "a", v: 1},
                {_id: 1, t: t1, m: "a", v: 2},
            ]),
        );

        const bucketsColl = getTimeseriesCollForRawOps(testDB, coll);
        const bucketsBefore = bucketsColl.find().rawData().toArray();
        assert.eq(1, bucketsBefore.length, "expected one bucket: " + tojson(bucketsBefore));
        const bucketId = bucketsBefore[0]._id;

        // Still <= every measurement in the bucket, so the other validator checks still hold; only
        // the _id <-> control.min.t equality check would object, and that's disabled by
        // timeseriesLessStrictBucketValidator.
        const newMinTime = ISODate("2024-01-01T00:00:00Z");

        const res = bucketsColl.updateOne(
            {_id: bucketId},
            {$set: {"control.min.t": newMinTime}},
            getRawOperationSpec(testDB),
        );
        assert.eq(1, res.matchedCount, "expected one bucket to match the update: " + tojson(res));
        assert.eq(1, res.modifiedCount, "expected one bucket to be modified: " + tojson(res));

        const bucketsAfter = bucketsColl.find().rawData().toArray();
        assert.eq(1, bucketsAfter.length);
        assert.eq(
            newMinTime,
            bucketsAfter[0].control.min.t,
            "control.min.t should have been changed by the rawData update",
        );

        // Drop the collection before teardown -- ReplSetTest.stopSet() validates collections, and
        // the bucket we produced has a control.min.t that no longer matches its _id's embedded
        // timestamp (we deliberately bypassed that check above), which validation would flag.
        assert(coll.drop());
    });
});
