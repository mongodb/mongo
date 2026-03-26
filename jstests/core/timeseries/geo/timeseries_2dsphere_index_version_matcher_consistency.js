/**
 * Tests that the geo matchers use the same 2dsphere index version semantics as the query's index
 * when filtering buckets. This prevents index-returned buckets from being incorrectly discarded
 * when control min/max are parsed with a different geometry type order.
 *
 * Background: For object-type geo elements that can be parsed as both legacy point and GeoJSON,
 * V3 tries legacy first; V4 tries GeoJSON first. If the matcher used the wrong version, a bucket
 * correctly returned by the index could be filtered out (or vice versa). This test uses an
 * "ambiguous" document {x: 0, y: 0, type: "Point", coordinates: [10, 10]} which parses as (0,0)
 * under V3 and (10,10) under V4.
 *
 * Also confirms the geo matcher works without an index (collection scan path uses default V4
 * semantics when no index version is available).
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_pipeline_optimization,
 *   requires_timeseries,
 *   featureFlag2dsphereIndexVersion4,
 *   cannot_run_during_upgrade_downgrade,
 *   multiversion_incompatible,
 * ]
 */

import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";
import {describe, it, before} from "jstests/libs/mochalite.js";

const timeFieldName = "time";
const metaFieldName = "meta";

// Polygon that contains (0,0) but not (10,10). Used with V3 index + ambiguous doc.
const polygonAroundZeroZero = {
    type: "Polygon",
    coordinates: [
        [
            [0, 0],
            [0.5, 0],
            [0.5, 0.5],
            [0, 0.5],
            [0, 0],
        ],
    ],
};

// Polygon that contains (10,10) but not (0,0). Used with V4 index or no index + ambiguous doc.
const polygonAroundTenTen = {
    type: "Polygon",
    coordinates: [
        [
            [9.5, 9.5],
            [10.5, 9.5],
            [10.5, 10.5],
            [9.5, 10.5],
            [9.5, 9.5],
        ],
    ],
};

// Ambiguous geo: V3 parses as legacy point (0,0), V4 parses as GeoJSON Point (10,10).
// BSON key order preserves "x" first, so firstElement().isNumber() is true for V3.
const ambiguousLoc = {x: 0, y: 0, type: "Point", coordinates: [10, 10]};

const now = new Date();

describe("2dsphere index version / matcher consistency", function () {
    before(function () {
        this.testDb = db.getSiblingDB(jsTestName());
        this.coll = this.testDb.getCollection(jsTestName());
        this.resetCollection = function (indexOptions = {}) {
            this.coll.drop();
            assert.commandWorked(
                this.testDb.createCollection(this.coll.getName(), {
                    timeseries: {timeField: timeFieldName, metaField: metaFieldName},
                }),
            );
            if (Object.keys(indexOptions).length > 0) {
                assert.commandWorked(this.coll.createIndex({loc: "2dsphere"}, indexOptions));
            }
        };
    });

    // V3 index + polygon around (0,0). The bucket-level matcher must use V3 semantics so the
    // ambiguous doc is parsed as (0,0), lies inside the polygon, and the bucket is kept.
    // Expected: one result. If the matcher used V4, it would parse as (10,10) and wrongly discard.
    it("V3 2dsphere index: matcher uses V3 so bucket with ambiguous loc is kept", function () {
        this.resetCollection({"2dsphereIndexVersion": 3});
        assert.commandWorked(
            this.coll.insert([{[timeFieldName]: now, [metaFieldName]: {s: 1}, _id: 1, loc: ambiguousLoc}]),
        );
        const results = this.coll
            .aggregate([{$match: {loc: {$geoWithin: {$geometry: polygonAroundZeroZero}}}}])
            .toArray();
        assert.eq(results.length, 1, "With V3 index, matcher must parse control as (0,0)");
        assert.docEq(ambiguousLoc, results[0].loc);
    });

    // V4 index + polygon around (10,10). The matcher must use V4 semantics so the ambiguous doc
    // is parsed as (10,10), lies inside the polygon, and the bucket is kept.
    // Expected: one result. If the matcher used V3, it would parse as (0,0) and wrongly discard.
    it("V4 2dsphere index: matcher uses V4 so bucket with ambiguous loc is kept", function () {
        this.resetCollection({"2dsphereIndexVersion": 4});
        assert.commandWorked(
            this.coll.insert([{[timeFieldName]: now, [metaFieldName]: {s: 1}, _id: 2, loc: ambiguousLoc}]),
        );
        const results = this.coll
            .aggregate([{$match: {loc: {$geoWithin: {$geometry: polygonAroundTenTen}}}}])
            .toArray();
        assert.eq(results.length, 1, "With V4 index, matcher must parse control as (10,10)");
        assert.docEq(ambiguousLoc, results[0].loc);
    });

    // No 2dsphere index: the server cannot infer an index version, so the matcher uses default
    // V4 semantics. Query uses polygon around (10,10); ambiguous doc must parse as (10,10).
    // Expected: one result. Ensures collection-scan path behaves consistently (V4).
    it("No index (collection scan): matcher uses default V4 semantics", function () {
        this.resetCollection();
        assert.commandWorked(
            this.coll.insert([{[timeFieldName]: now, [metaFieldName]: {s: 1}, _id: 3, loc: ambiguousLoc}]),
        );
        const results = this.coll
            .aggregate([{$match: {loc: {$geoWithin: {$geometry: polygonAroundTenTen}}}}])
            .toArray();
        assert.eq(results.length, 1, "Without index, matcher defaults to V4");
        assert.docEq(ambiguousLoc, results[0].loc);
    });

    // V4 index + polygon around (0,0). With V4, ambiguous doc is (10,10), which is outside the
    // polygon. The matcher must use V4 so the bucket is correctly excluded.
    // Expected: zero results. If the matcher wrongly used V3, it would keep the bucket (false positive).
    it("V4 index: polygon around (0,0) must not match ambiguous doc", function () {
        this.resetCollection({"2dsphereIndexVersion": 4});
        assert.commandWorked(
            this.coll.insert([{[timeFieldName]: now, [metaFieldName]: {s: 1}, _id: 4, loc: ambiguousLoc}]),
        );
        const results = this.coll
            .aggregate([{$match: {loc: {$geoWithin: {$geometry: polygonAroundZeroZero}}}}])
            .toArray();
        assert.eq(results.length, 0, "V4 parses as (10,10); polygon does not contain (10,10)");
    });

    // V3 index + polygon around (10,10). With V3, ambiguous doc is (0,0), which is outside the
    // polygon. The matcher must use V3 so the bucket is correctly excluded.
    // Expected: zero results. If the matcher wrongly used V4, it would keep the bucket (false positive).
    it("V3 index: polygon around (10,10) must not match ambiguous doc", function () {
        this.resetCollection({"2dsphereIndexVersion": 3});
        assert.commandWorked(
            this.coll.insert([{[timeFieldName]: now, [metaFieldName]: {s: 1}, _id: 5, loc: ambiguousLoc}]),
        );
        const results = this.coll
            .aggregate([{$match: {loc: {$geoWithin: {$geometry: polygonAroundTenTen}}}}])
            .toArray();
        assert.eq(results.length, 0, "V3 parses as (0,0); polygon does not contain (0,0)");
    });

    // $and of $geoWithin (polygon around 0,0) and a meta filter. With a V3 index, the event-level
    // filter (after unpacking) must use V3 to parse the ambiguous doc as (0,0), so it matches.
    // Expected: one result. Confirms the version from the index is applied to the event filter in
    // $and.
    it("V3 index with $and: geo + meta filter uses V3 for event filter", function () {
        this.resetCollection({"2dsphereIndexVersion": 3});
        assert.commandWorked(
            this.coll.insert([{[timeFieldName]: now, [metaFieldName]: {s: 1}, _id: 6, loc: ambiguousLoc}]),
        );
        const results = this.coll
            .aggregate([
                {
                    $match: {
                        $and: [{loc: {$geoWithin: {$geometry: polygonAroundZeroZero}}}, {[metaFieldName]: {s: 1}}],
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 1, "With V3 index, $and with geo must parse event loc as (0,0)");
        assert.docEq(ambiguousLoc, results[0].loc);
    });

    // $and of $geoWithin (polygon around 10,10) and a meta filter. With a V4 index, the
    // event-level filter must use V4 to parse the ambiguous doc as (10,10), so it matches.
    // Expected: one result. Confirms the version from the index is applied to the event filter in
    // $and.
    it("V4 index with $and: geo + meta filter uses V4 for event filter", function () {
        this.resetCollection({"2dsphereIndexVersion": 4});
        assert.commandWorked(
            this.coll.insert([{[timeFieldName]: now, [metaFieldName]: {s: 1}, _id: 7, loc: ambiguousLoc}]),
        );
        const results = this.coll
            .aggregate([
                {
                    $match: {
                        $and: [{loc: {$geoWithin: {$geometry: polygonAroundTenTen}}}, {[metaFieldName]: {s: 1}}],
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 1, "With V4 index, $and with geo must parse event loc as (10,10)");
        assert.docEq(ambiguousLoc, results[0].loc);
    });

    // $or of two $geoWithin (polygon 0,0 and polygon 10,10). With V3, the ambiguous doc is (0,0),
    // so only the first branch matches. The event filter for each branch must use V3.
    // Expected: one result. Confirms $or uses the same index version for all branches.
    it("V3 index with $or: ambiguous doc matches first branch (polygon around 0,0)", function () {
        this.resetCollection({"2dsphereIndexVersion": 3});
        assert.commandWorked(
            this.coll.insert([{[timeFieldName]: now, [metaFieldName]: {s: 1}, _id: 8, loc: ambiguousLoc}]),
        );
        const results = this.coll
            .aggregate([
                {
                    $match: {
                        $or: [
                            {loc: {$geoWithin: {$geometry: polygonAroundZeroZero}}},
                            {loc: {$geoWithin: {$geometry: polygonAroundTenTen}}},
                        ],
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 1, "With V3 index, $or must use V3 for event filter so first branch (0,0) matches");
        assert.docEq(ambiguousLoc, results[0].loc);
    });

    // $or of two $geoWithin (polygon 0,0 and polygon 10,10). With V4, the ambiguous doc is (10,10),
    // so only the second branch matches. The event filter for each branch must use V4.
    // Expected: one result. Confirms $or uses the same index version for all branches.
    it("V4 index with $or: ambiguous doc matches second branch (polygon around 10,10)", function () {
        this.resetCollection({"2dsphereIndexVersion": 4});
        assert.commandWorked(
            this.coll.insert([{[timeFieldName]: now, [metaFieldName]: {s: 1}, _id: 9, loc: ambiguousLoc}]),
        );
        const results = this.coll
            .aggregate([
                {
                    $match: {
                        $or: [
                            {loc: {$geoWithin: {$geometry: polygonAroundZeroZero}}},
                            {loc: {$geoWithin: {$geometry: polygonAroundTenTen}}},
                        ],
                    },
                },
            ])
            .toArray();
        assert.eq(
            results.length,
            1,
            "With V4 index, $or must use V4 for event filter so second branch (10,10) matches",
        );
        assert.docEq(ambiguousLoc, results[0].loc);
    });
});
