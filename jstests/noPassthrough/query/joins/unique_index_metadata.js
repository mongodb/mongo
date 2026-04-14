/**
 * This test demonstrates that the join optimizer is capable of using index metadata during join
 * cardinality estimation to improve cardinality estimation.
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe
 * ]
 */

import {joinOptUsed} from "jstests/libs/query/join_utils.js";
import {getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

let conn = MongoRunner.runMongod({setParameter: {featureFlagPathArrayness: true}});
const db = conn.getDB("test");
const coll1 = db[jsTestName()];
const coll2 = db[jsTestName() + "_2"];
coll1.drop();
coll2.drop();

// coll1 is our large base collection with a "foreign key" to coll2 on field "b".
let docs1 = Array.from({length: 1000}, (_, i) => ({_id: i, b: i % 100}));
assert.commandWorked(coll1.insertMany(docs1));
// Add index for multikeyness info for path arrayness.
assert.commandWorked(coll1.createIndex({dummy: 1, b: 1}));

let docs2 = Array.from({length: 100}, (_, i) => ({_id: i, b: i}));
assert.commandWorked(coll2.insertMany(docs2));
assert.commandWorked(coll2.createIndex({b: 1}, {unique: true}));
// Add index for multikeyness info for path arrayness.
assert.commandWorked(coll2.createIndex({dummy: 1, b: 1}));

const pipeline = [
    {
        $lookup: {
            from: coll2.getName(),
            localField: "b",
            foreignField: "b",
            as: "coll2",
        },
    },
    {
        $unwind: "$coll2",
    },
];

// Enable join optimization and use of unique index metadata. Also set other knobs intentionally
// to make the sample size small, which normally would cause CE fluctuations.
assert.commandWorked(
    conn.adminCommand({
        setParameter: 1,
        internalEnableJoinOptimization: true,
        internalEnableJoinOptimizationUseIndexUniqueness: true,
        internalJoinPlanSamplingSize: 100,
        internalQuerySamplingBySequentialScan: false,
    }),
);

// No matter how many times we run this pipeline, we will get exactly the same cardinality estimate
// for the join due to the use of unique index metadata.
for (let i = 0; i < 10; i++) {
    const uniqueExplain = coll1.explain().aggregate(pipeline);
    assert(joinOptUsed(uniqueExplain), "Join optimizer was not used as expected: " + tojson(uniqueExplain));

    const uniqueWinningPlan = getWinningPlanFromExplain(uniqueExplain);
    assert(
        uniqueWinningPlan.hasOwnProperty("cardinalityEstimate"),
        "Cardinality estimate missing from explain: " + tojson(uniqueExplain),
    );

    assert.eq(
        uniqueWinningPlan.cardinalityEstimate,
        1000,
        "Cardinality estimate was not equal to the expected value of 1000: " + tojson(uniqueExplain),
    );
    const result = coll1.aggregate(pipeline).toArray();
    assert.eq(
        result.length,
        1000,
        "Expected 1000 results from the join, but did not, with prior explain" + tojson(uniqueExplain),
    );
}

// With the knob off, the query still runs to completion.
assert.commandWorked(
    conn.adminCommand({
        setParameter: 1,
        internalEnableJoinOptimizationUseIndexUniqueness: false,
    }),
);
const explain = coll1.explain().aggregate(pipeline);
assert(joinOptUsed(explain), "Join optimizer was not used as expected: " + tojson(explain));
const result = coll1.aggregate(pipeline).toArray();
assert.eq(
    result.length,
    1000,
    "Expected 1000 results from the join, but did not, with prior explain" + tojson(explain),
);

MongoRunner.stopMongod(conn);
