/**
 * A $scoreFusion/$rankFusion inside a user-written $unionWith targeting a view must work on a
 * sharded cluster: the hybrid stage LP-desugars inside the $unionWith and view resolution must
 * resolve the resulting inner $unionWith against the view's underlying collection. No mongot
 * needed (uses $score inputs).
 *
 * @tags: [
 *   featureFlagRankFusionFull,
 *   featureFlagSearchHybridScoringFull,
 *   requires_fcv_90,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("hybrid search inside $unionWith on a sharded view", function () {
    let st;
    let testDB;

    const scoreFusion = {
        $scoreFusion: {
            input: {
                pipelines: {
                    a: [{$score: {score: "$x", normalization: "minMaxScaler"}}, {$sort: {x: -1}}],
                    b: [{$score: {score: "$y", normalization: "minMaxScaler"}}, {$sort: {y: -1}}],
                },
                normalization: "none",
            },
            combination: {method: "avg"},
        },
    };
    const rankFusion = {
        $rankFusion: {input: {pipelines: {a: [{$sort: {x: -1}}], b: [{$sort: {y: -1}}]}}},
    };

    before(function () {
        st = new ShardingTest({shards: 2, mongos: 1});
        testDB = st.s.getDB("test");
        assert.commandWorked(
            testDB.base.insertMany([
                {_id: 0, x: 1, y: 9},
                {_id: 1, x: 5, y: 4},
                {_id: 2, x: 3, y: 7},
                {_id: 3, x: 8, y: 2},
            ]),
        );
        st.shardColl(testDB.base, {_id: 1}, false);
        assert.commandWorked(testDB.createView("baseView", "base", [{$match: {x: {$gte: 0}}}]));
        assert.commandWorked(testDB.outer.insertMany([{_id: 0}]));
    });

    after(function () {
        st.stop();
    });

    function assertUnionWithHybridSucceeds(stage, name) {
        const pipeline = [{$unionWith: {coll: "baseView", pipeline: [stage]}}];
        const res = assert.commandWorked(
            testDB.runCommand({aggregate: "outer", pipeline, cursor: {}}),
            `${name} inside $unionWith on a sharded view should succeed`,
        );
        // outer's single doc, plus the 4 view docs the hybrid stage ranks/scores.
        assert.eq(5, res.cursor.firstBatch.length, `${name}: unexpected result count`, {res});
    }

    it("$scoreFusion succeeds", function () {
        assertUnionWithHybridSucceeds(scoreFusion, "$scoreFusion");
    });

    it("$rankFusion succeeds", function () {
        assertUnionWithHybridSucceeds(rankFusion, "$rankFusion");
    });
});
