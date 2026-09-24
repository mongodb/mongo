/**
 * Tests that a residual predicate above a $geoNear stage is only evaluated on the documents that
 * the near search actually returns. The planner pushes such a predicate into the $geoNear node
 * ('docFilter'), and the near stage must evaluate it after its distance check. Otherwise a
 * predicate that throws -- for instance an $expr with a failing $convert -- would fail the whole
 * query because of a document that is not part of the result set at all.
 *
 * Also verifies that the pushdown does not change query results, for both 2d and 2dsphere indexes
 * and for both the aggregation and the find entry points.
 *
 * @tags: [
 *   # $geoNear is not supported on views.
 *   incompatible_with_views,
 *   # $geoNear must be the first stage in a pipeline.
 *   assumes_unsharded_collection,
 *   requires_fcv_91,
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */

import {describe, it, before} from "jstests/libs/mochalite.js";

// A predicate that throws when it is evaluated on a document whose field is not a valid ObjectId.
function throwsUnlessObjectId(field) {
    return {$expr: {$toObjectId: "$" + field}};
}

function ids(results) {
    return results.map((doc) => doc._id);
}

describe("$geoNear residual filter pushdown with a 2dsphere index", function () {
    const coll = db[jsTestName() + "_2dsphere"];

    // Distances from the query point [0, 0], in meters:
    //   _id 0: 0
    //   _id 1: ~157000
    //   _id 2: ~314000
    // Field 's' is not a valid ObjectId on the nearest document, and 't' is not a valid ObjectId on
    // the farthest one, so each of the two distance bounds can be tested with a throwing predicate.
    const kValidOid = "000000000000000000000000";

    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
        assert.commandWorked(
            coll.insert([
                {
                    _id: 0,
                    loc: {type: "Point", coordinates: [0, 0]},
                    s: "",
                    t: kValidOid,
                    k: 1,
                    c: "abc",
                },
                {
                    _id: 1,
                    loc: {type: "Point", coordinates: [1, 1]},
                    s: kValidOid,
                    t: kValidOid,
                    k: 2,
                    c: "ABC",
                },
                {
                    _id: 2,
                    loc: {type: "Point", coordinates: [2, 2]},
                    s: kValidOid,
                    t: "",
                    k: 3,
                    c: "xyz",
                },
            ]),
        );
    });

    function geoNear(spec) {
        return coll
            .aggregate([
                {
                    $geoNear: Object.assign(
                        {near: {type: "Point", coordinates: [0, 0]}, distanceField: "dist"},
                        spec,
                    ),
                },
                {$project: {_id: 1}},
            ])
            .toArray();
    }

    it("does not evaluate the predicate on documents closer than minDistance", function () {
        const res = geoNear({minDistance: 100000, query: throwsUnlessObjectId("s")});
        assert.eq([1, 2], ids(res), "unexpected results", {res});
    });

    it("does not evaluate the predicate on documents farther than maxDistance", function () {
        // The farthest document would make the predicate throw, but it is outside the search
        // bounds, so the predicate never sees it.
        const res = geoNear({maxDistance: 250000, query: throwsUnlessObjectId("t")});
        assert.eq([0, 1], ids(res), "unexpected results", {res});
    });

    it("respects both distance bounds at once", function () {
        const res = geoNear({
            minDistance: 100000,
            maxDistance: 250000,
            query: {$and: [throwsUnlessObjectId("s"), throwsUnlessObjectId("t")]},
        });
        assert.eq([1], ids(res), "unexpected results", {res});
    });

    it("applies a non-throwing residual predicate correctly", function () {
        const res = geoNear({query: {k: {$gte: 2}}});
        assert.eq([1, 2], ids(res), "unexpected results", {res});
    });

    it("returns nothing when the predicate matches nothing", function () {
        assert.eq([], geoNear({query: {k: 999}}));
    });

    it("returns the same documents as an equivalent $match above the stage", function () {
        const pushedDown = coll
            .aggregate([
                {
                    $geoNear: {
                        near: {type: "Point", coordinates: [0, 0]},
                        distanceField: "dist",
                        query: {k: {$gte: 2}},
                    },
                },
                {$project: {_id: 1}},
            ])
            .toArray();
        const notPushedDown = coll
            .aggregate([
                {$geoNear: {near: {type: "Point", coordinates: [0, 0]}, distanceField: "dist"}},
                {$match: {k: {$gte: 2}}},
                {$project: {_id: 1}},
            ])
            .toArray();
        assert.eq(pushedDown, notPushedDown, "pushdown changed the results", {
            pushedDown,
            notPushedDown,
        });
    });

    it("applies a limit to the documents that pass the predicate", function () {
        const res = coll
            .aggregate([
                {
                    $geoNear: {
                        near: {type: "Point", coordinates: [0, 0]},
                        distanceField: "dist",
                        query: {k: {$gte: 2}},
                    },
                },
                {$limit: 1},
                {$project: {_id: 1}},
            ])
            .toArray();
        assert.eq([1], ids(res), "unexpected results", {res});
    });

    it("respects the collation of the query", function () {
        const res = coll
            .aggregate(
                [
                    {
                        $geoNear: {
                            near: {type: "Point", coordinates: [0, 0]},
                            distanceField: "dist",
                            query: {c: "abc"},
                        },
                    },
                    {$project: {_id: 1}},
                ],
                {collation: {locale: "en_US", strength: 2}},
            )
            .toArray();
        assert.eq([0, 1], ids(res), "unexpected results", {res});
    });

    it("works for a $nearSphere query issued through find", function () {
        const res = coll
            .find(
                {
                    loc: {
                        $nearSphere: {
                            $geometry: {type: "Point", coordinates: [0, 0]},
                            $minDistance: 100000,
                        },
                    },
                    ...throwsUnlessObjectId("s"),
                },
                {_id: 1},
            )
            .toArray();
        assert.eq([1, 2], ids(res), "unexpected results", {res});
    });

    it("works for a $near query issued through find", function () {
        const res = coll
            .find(
                {
                    loc: {
                        $near: {
                            $geometry: {type: "Point", coordinates: [0, 0]},
                            $maxDistance: 250000,
                        },
                    },
                    ...throwsUnlessObjectId("t"),
                },
                {_id: 1},
            )
            .toArray();
        assert.eq([0, 1], ids(res), "unexpected results", {res});
    });
});

describe("$geoNear residual filter pushdown with a 2d index", function () {
    const coll = db[jsTestName() + "_2d"];
    const kValidOid = "000000000000000000000000";

    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({loc: "2d"}));
        // At distance 0 from the query point below, so it is rejected by 'minDistance'. Its 's'
        // field is not a valid ObjectId, so evaluating the residual predicate on it throws.
        assert.commandWorked(coll.insert({_id: 0, loc: [0, 0], s: "", t: kValidOid, k: 1}));
        // Roughly 0.0247 radians away from the query point, so it is inside the annulus.
        assert.commandWorked(coll.insert({_id: 1, loc: [1, 1], s: kValidOid, t: kValidOid, k: 2}));
        // Roughly 0.0494 radians away, outside of the 'maxDistance' used below.
        assert.commandWorked(coll.insert({_id: 2, loc: [2, 2], s: kValidOid, t: "", k: 3}));
        // A multikey document: the same document is a candidate in more than one search interval,
        // which exercises the near stage's deduplication together with the residual predicate.
        assert.commandWorked(
            coll.insert({
                _id: 3,
                loc: [
                    [0.5, 0.5],
                    [3, 3],
                ],
                s: kValidOid,
                t: kValidOid,
                k: 2,
            }),
        );
    });

    function geoNear(spec) {
        return coll
            .aggregate([
                {
                    $geoNear: Object.assign(
                        {near: [0, 0], distanceField: "dist", spherical: true},
                        spec,
                    ),
                },
                {$project: {_id: 1}},
            ])
            .toArray();
    }

    it("does not evaluate the predicate on documents closer than minDistance", function () {
        const res = geoNear({minDistance: 0.01, query: throwsUnlessObjectId("s")});
        assert.eq([1, 2, 3], ids(res).sort(), "unexpected results", {res});
    });

    it("does not evaluate the predicate on documents farther than maxDistance", function () {
        const res = geoNear({maxDistance: 0.03, query: throwsUnlessObjectId("t")});
        assert.eq([0, 3, 1], ids(res), "unexpected results", {res});
    });

    it("still applies the predicate to documents within the search bounds", function () {
        const res = geoNear({minDistance: 0.01, query: {$expr: {$eq: ["$_id", 999]}}});
        assert.eq([], res, "unexpected results", {res});
    });

    it("applies a non-throwing residual predicate correctly without minDistance", function () {
        const res = geoNear({query: {s: {$ne: ""}}});
        assert.eq([3, 1, 2], ids(res), "unexpected results", {res});
    });

    it("returns a matching multikey document exactly once", function () {
        const res = geoNear({query: {k: 2}});
        assert.eq([3, 1], ids(res), "unexpected results", {res});
    });

    it("does not return a multikey document rejected by the predicate", function () {
        const res = geoNear({query: {_id: {$ne: 3}}});
        assert.eq([0, 1, 2], ids(res), "unexpected results", {res});
    });

    it("returns the same documents as an equivalent $match above the stage", function () {
        const pushedDown = geoNear({query: {k: {$gte: 2}}});
        const notPushedDown = coll
            .aggregate([
                {$geoNear: {near: [0, 0], distanceField: "dist", spherical: true}},
                {$match: {k: {$gte: 2}}},
                {$project: {_id: 1}},
            ])
            .toArray();
        assert.eq(pushedDown, notPushedDown, "pushdown changed the results", {
            pushedDown,
            notPushedDown,
        });
    });

    it("works for a $near query issued through find", function () {
        const res = coll.find({loc: {$near: [0, 0]}, k: 2}, {_id: 1}).toArray();
        assert.eq([3, 1], ids(res), "unexpected results", {res});
    });

    it("works for a $nearSphere query issued through find", function () {
        // $nearSphere on a 2d index uses the same GeoNear2DStage path as $near, only with a
        // spherical distance metric. The residual predicate must still be applied after the distance
        // check.
        const res = coll
            .find({loc: {$nearSphere: [0, 0], $maxDistance: 0.03}, k: 2}, {_id: 1})
            .toArray();
        assert.eq([3, 1], ids(res), "unexpected results", {res});
    });

    it("does not evaluate the predicate on documents outside $nearSphere bounds", function () {
        // _id 2 is outside of the $maxDistance and would make the predicate throw.
        const res = coll
            .find(
                {
                    loc: {$nearSphere: [0, 0], $maxDistance: 0.03},
                    ...throwsUnlessObjectId("t"),
                },
                {_id: 1},
            )
            .toArray();
        assert.eq([0, 3, 1], ids(res), "unexpected results", {res});
    });
});
