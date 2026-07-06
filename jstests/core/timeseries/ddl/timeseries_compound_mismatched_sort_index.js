/**
 * Ensures that a compound rawData index with mismatched control.min/max sort
 * directions should not cause wrong query results when hinted.
 *
 * @tags: [
 *   requires_fcv_82,
 *   requires_timeseries,
 *   does_not_support_stepdowns,
 * ]
 */

import {describe, it} from "jstests/libs/mochalite.js";

describe("compound rawData index with mismatched min/max sort directions", function () {
    it("returns correct results when hinted", function () {
        const coll = db.getCollection(jsTestName());
        coll.drop();

        assert.commandWorked(
            db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "m"}}),
        );

        // Insert three documents into separate buckets (different meta values) so each document
        // lands in its own bucket. This ensures the index hint exercises bucket pruning.
        assert.commandWorked(
            coll.insertMany([
                {t: new Date("2024-01-01T00:00:00Z"), m: "a", x: 1},
                {t: new Date("2024-01-01T00:00:00Z"), m: "b", x: 2},
                {t: new Date("2024-01-01T00:01:00Z"), m: "c", x: 3},
            ]),
        );

        // Baseline: unhinted query returns all 3 documents.
        assert.eq(3, coll.find({x: {$gte: 1}}).toArray().length, "baseline (no hint)");

        // Create a compound rawData index where control.min and control.max have opposite sort
        // directions. This is a non-conforming index spec.
        assert.commandWorked(
            db.runCommand({
                createIndexes: coll.getName(),
                indexes: [
                    {key: {"control.min.x": 1, "control.max.x": -1}, name: "mismatched_order_x"},
                ],
                rawData: true,
            }),
        );

        // Hinted query must still return all 3 documents. Before the fix this returned 2 because
        // the mismatched sort directions caused the bucket predicate to incorrectly prune a bucket.
        assert.eq(
            3,
            coll
                .find({x: {$gte: 1}})
                .hint("mismatched_order_x")
                .toArray().length,
            "hinted query with mismatched rawData index",
        );
    });
});
