/**
 * Tests a $geoNear query with a residual predicate against a sharded collection. The planner pushes
 * such a predicate into the $geoNear node and removes the FETCH stage that used to sit above it, so
 * the SHARDING_FILTER stage ends up directly above the $geoNear stage. This verifies that the
 * predicate is applied on every shard, that it does not change the result set, and that it is not
 * evaluated on documents which the distance bounds exclude.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st;
let coll;

before(function () {
    st = new ShardingTest({shards: 2});
    const db = st.s.getDB("test");
    coll = db.geo_near_residual_filter_pushdown;

    assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));
    assert.commandWorked(
        st.s.adminCommand({shardCollection: coll.getFullName(), key: {shardKey: 1}}),
    );
    assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {shardKey: 10}}));
    assert.commandWorked(
        st.s.adminCommand({
            moveChunk: coll.getFullName(),
            find: {shardKey: 10},
            to: st.shard1.shardName,
        }),
    );

    // 20 documents, half on each shard, at increasing distance from [0, 0].
    const docs = [];
    for (let i = 0; i < 20; ++i) {
        docs.push({
            _id: i,
            shardKey: i,
            loc: {type: "Point", coordinates: [i / 10, i / 10]},
            k: i % 2,
            // Not a valid ObjectId on the document nearest to the query point, so a predicate
            // converting 's' throws if it is evaluated on a document that 'minDistance' excludes.
            s: i === 0 ? "" : "000000000000000000000000",
            // Likewise for the farthest document and 'maxDistance'.
            t: i === 19 ? "" : "000000000000000000000000",
        });
    }
    assert.commandWorked(coll.insert(docs));
    assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
});

after(function () {
    st.stop();
});

describe("$geoNear residual filter pushdown on a sharded collection", function () {
    function geoNear(query, extraSpec = {}) {
        return coll
            .aggregate([
                {
                    $geoNear: Object.assign(
                        {
                            near: {type: "Point", coordinates: [0, 0]},
                            distanceField: "dist",
                            query: query,
                        },
                        extraSpec,
                    ),
                },
                {$project: {_id: 1}},
            ])
            .toArray()
            .map((doc) => doc._id);
    }

    it("applies the residual predicate across shards", function () {
        assert.eq([1, 3, 5, 7, 9, 11, 13, 15, 17, 19], geoNear({k: 1}), "unexpected results");
    });

    it("returns the same documents as an equivalent $match above the stage", function () {
        const notPushedDown = coll
            .aggregate([
                {$geoNear: {near: {type: "Point", coordinates: [0, 0]}, distanceField: "dist"}},
                {$match: {k: 1}},
                {$project: {_id: 1}},
            ])
            .toArray()
            .map((doc) => doc._id);
        assert.eq(geoNear({k: 1}), notPushedDown, "pushdown changed the results");
    });

    it("does not evaluate the predicate on documents farther than maxDistance", function () {
        // The farthest document would make the predicate throw, but it is outside the search bounds
        // on its shard, so the predicate never sees it.
        const res = geoNear({$expr: {$toObjectId: "$t"}}, {maxDistance: 250000});
        assert.eq(
            [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
            res,
            "unexpected results",
        );
    });

    it("does not evaluate the predicate on documents closer than minDistance", function () {
        // The nearest document would make the predicate throw, but it is excluded by 'minDistance'
        // on its shard, so the predicate never sees it.
        const res = geoNear({$expr: {$toObjectId: "$s"}}, {minDistance: 1000});
        assert.eq(
            [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19],
            res,
            "unexpected results",
        );
    });
});
