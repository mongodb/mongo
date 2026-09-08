/**
 * Tests that no measurements are lost when a former primary of a donor shard steps back up while
 * the range deletion for a donated chunk is still pending.
 *
 * The bucket catalog clear on chunk donation (MigrationSourceManager) runs only on the node that
 * is primary when the migration commits, and other nodes' bucket catalogs are only fenced by the
 * range deleter's replicated deletes. Without also clearing the bucket catalog when the range
 * deletion task becomes ready, a node which steps up before the range deletion runs still holds an
 * open bucket for the donated range, and stages a correctly-routed insert into it. The measurement
 * is then written only into the orphaned bucket document, which the range deleter destroys: silent
 * data loss.
 *
 * @tags: [requires_sharding, requires_fcv_82, multiversion_incompatible]
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("time-series writes on a stepped-up donor with a pending range deletion", function () {
    const dbName = "test";
    const collName = jsTestName();
    const timeField = "t";
    const metaField = "m";

    // A chunk boundary that is deliberately not aligned to the bucket rounding granularity, so
    // that measurements routed by their rounded bucket min can fall close to either side of it.
    const boundary = ISODate("2000-01-25T00:00:01.021Z");
    // Rounds down to a bucket min below the boundary: belongs to the lower chunk.
    const belowBoundary = new Date(boundary.getTime() - 10 * 60 * 1000);
    // Rounds down to a bucket min above the boundary, so mongos routes it to the owner of the
    // upper chunk, but it is within bucketMaxSpanSeconds (1h) of the bucket opened for
    // 'belowBoundary', so a stale bucket catalog would stage it into that bucket.
    const aboveBoundary = new Date(boundary.getTime() + 5 * 60 * 1000);

    const rawBucketsOn = (node, filter) => {
        const nodeDb = node.getDB(dbName);
        const rawColl = getTimeseriesCollForRawOps(nodeDb, nodeDb[collName]);
        const res = assert.commandWorked(
            nodeDb.runCommand(
                Object.assign(
                    {find: rawColl.getName(), filter: filter},
                    getRawOperationSpec(nodeDb),
                ),
            ),
        );
        return res.cursor.firstBatch;
    };

    let st;
    let coll;
    let bucketsNs;
    let nodeA;
    let nodeB;

    before(function () {
        st = new ShardingTest({shards: 2, rs: {nodes: 2}});
        const db = st.s.getDB(dbName);
        coll = db[collName];

        assert.commandWorked(db.createCollection(collName, {timeseries: {timeField, metaField}}));
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
        assert.commandWorked(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: {[timeField]: 1}}),
        );

        bucketsNs = getTimeseriesCollForDDLOps(db, coll).getFullName();
        assert.commandWorked(
            st.s.adminCommand({split: bucketsNs, middle: {"control.min.t": boundary}}),
        );

        // Park both chunks on shard1, the donor under test.
        for (const bounds of [
            [{"control.min.t": MinKey}, {"control.min.t": boundary}],
            [{"control.min.t": boundary}, {"control.min.t": MaxKey}],
        ]) {
            assert.commandWorked(
                st.s.adminCommand({
                    moveChunk: bucketsNs,
                    bounds,
                    to: st.shard1.shardName,
                    _waitForDelete: true,
                }),
            );
        }

        nodeA = st.rs1.getPrimary();
        nodeB = st.rs1.getSecondary();
    });

    after(function () {
        st.stop();
    });

    it("does not lose measurements written after the step-up", function () {
        // Open a bucket on node A whose control.min.t is in the lower chunk.
        assert.commandWorked(coll.insert({_id: 0, [metaField]: 0, [timeField]: belowBoundary}));
        assert.eq(
            1,
            rawBucketsOn(nodeA, {"control.min.t": {$lt: boundary}}).length,
            "expected the bucket on node A",
        );

        // Suspend range deletion on both nodes so that we can guarantee node A steps back up before it runs
        const suspendOnA = configureFailPoint(nodeA, "suspendRangeDeletion");
        const suspendOnB = configureFailPoint(nodeB, "suspendRangeDeletion");

        try {
            // Step up node B so that the donation's bucket catalog clear runs on B, not A, then
            // donate the lower chunk, leaving its range deletion pending.
            st.rs1.stepUp(nodeB);
            assert.commandWorked(
                st.s.adminCommand({
                    moveChunk: bucketsNs,
                    bounds: [{"control.min.t": MinKey}, {"control.min.t": boundary}],
                    to: st.shard0.shardName,
                    _waitForDelete: false,
                }),
            );

            // Step A back up while it still holds the open bucket for the donated range, and
            // insert a measurement that mongos routes to shard1 (its rounded bucket min is above
            // the boundary) but that fits in that stale open bucket.
            st.rs1.stepUp(nodeA);
            assert.commandWorked(coll.insert({_id: 1, [metaField]: 0, [timeField]: aboveBoundary}));
        } finally {
            // Let the pending range deletion run. It must only remove the orphaned copy of the
            // cloned bucket; if the insert above was staged into the stale bucket, the measurement
            // with _id 1 exists only in the orphaned document and is destroyed with it.
            suspendOnA.off();
            suspendOnB.off();
        }

        assert.soon(
            () => rawBucketsOn(nodeA, {"control.min.t": {$lt: boundary}}).length === 0,
            "orphaned bucket was never cleaned up on shard1",
        );

        assert.sameMembers(
            [0, 1],
            coll
                .find({}, {_id: 1})
                .toArray()
                .map((doc) => doc._id),
            "a measurement was destroyed by the range deleter",
        );
    });
});
