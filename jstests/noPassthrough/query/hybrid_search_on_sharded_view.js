/**
 * $scoreFusion on a view on a sharded cluster (no mongot).
 *
 * @tags: [
 *   featureFlagRankFusionFull,
 *   featureFlagSearchHybridScoringFull,
 *   requires_fcv_90,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("$scoreFusion on a sharded view", function () {
    let st;
    let testDB;

    before(function () {
        st = new ShardingTest({shards: 2, mongos: 1});
        testDB = st.s.getDB("test");
        assert.commandWorked(
            testDB.coll.insertMany([
                {_id: 0, x: 1, y: 9},
                {_id: 1, x: 5, y: 4},
                {_id: 2, x: 3, y: 7},
                {_id: 3, x: 8, y: 2},
            ]),
        );
        st.shardColl(testDB.coll, {_id: 1}, false);
        assert.commandWorked(testDB.createView("collView", "coll", [{$match: {x: {$gte: 0}}}]));
    });

    after(function () {
        st.stop();
    });

    it("succeeds and returns every view document", function () {
        const pipeline = [
            {
                $scoreFusion: {
                    input: {
                        pipelines: {
                            a: [
                                {$score: {score: "$x", normalization: "minMaxScaler"}},
                                {$sort: {x: -1}},
                            ],
                            b: [
                                {$score: {score: "$y", normalization: "minMaxScaler"}},
                                {$sort: {y: -1}},
                            ],
                        },
                        normalization: "none",
                    },
                    combination: {method: "avg"},
                },
            },
        ];
        const res = assert.commandWorked(
            testDB.runCommand({aggregate: "collView", pipeline, cursor: {}}),
        );
        assert.eq(4, res.cursor.firstBatch.length, "expected all 4 docs from the view", {res});
    });
});
