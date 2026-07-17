/**
 * Ensure the cardinality estimate for joins that propagate single table predicates don't count said derived predicates.
 * @tags: [
 *   requires_sbe,
 *   requires_fcv_90,
 * ]
 */

import {getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {joinOptUsed} from "jstests/libs/query/join_utils.js";
import {checkJoinOptimizationStatus} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPathArrayness: true,
        internalEnableJoinOptimization: true,
    },
});
assert.neq(conn, null, "mongod failed to start");

const db = conn.getDB("test");

assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        internalQueryFrameworkControl: "trySbeEngine",
    }),
);

const coll = db.many_rows;
coll.drop();

const docs = [];
for (let i = 0; i < 1000; ++i) {
    docs.push({
        i_idx: i,
        i_noidx: i,
        i_idx_unique: i,
        c_idx: 1,
        d_idx: i % 10,
        i_idx_offset: i + 100000,
        n_idx: null,
    });
}
assert.commandWorked(coll.insertMany(docs));

// Only index needed for this regression.
assert.commandWorked(coll.createIndex({i_idx: 1}));

try {
    const pipeline = [
        {
            $lookup: {
                from: "many_rows",
                as: "right",
                let: {localField: "$i_idx"},
                pipeline: [
                    {
                        $match: {
                            $and: [{$expr: {$eq: ["$$localField", "$i_idx"]}}, {i_idx: 1}],
                        },
                    },
                ],
            },
        },
        {$unwind: "$right"},
        {$match: {}},
    ];

    const actualRes = coll.aggregate([...pipeline, {$count: "count"}]).toArray();
    const actualCount = actualRes.length ? actualRes[0].count : 0;

    const explain = coll.explain().aggregate(pipeline);
    assert(joinOptUsed(explain), "Join optimizer was not used: " + tojson(explain));

    const winningPlan = getWinningPlanFromExplain(explain);
    assert(
        winningPlan.hasOwnProperty("cardinalityEstimate"),
        "Missing cardinalityEstimate: " + tojson(explain),
    );

    const ce = winningPlan.cardinalityEstimate;

    jsTest.log(`Actual cardinality: ${actualCount}`);
    jsTest.log(`Estimated cardinality: ${ce}`);

    assert.eq(actualCount, 1, "Expected actual cardinality of 1: " + tojson(explain));
    assert.eq(ce, 1, "Expected CE of 1 for inferred-predicate regression: " + tojson(explain));
} finally {
}

MongoRunner.stopMongod(conn);
