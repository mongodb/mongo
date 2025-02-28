/*
 * Test that, in the presence of multiple plans and perfect histograms, CBR is able to select
 * the intuitively best plan to answer simple common queries. The invariants tested in this
 * test should hold regardless of any future calibration of the constants in the cost model.
 */

import {
    getPlanStage,
    getRejectedPlans,
    getWinningPlanFromExplain
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();

let docs = [];
// Unique values are inserted in unique1 and unique2 but the indexes over them are not
// defined as unique. If they were, the plan enumerator would always pick them heuristically,
// while we want CBR to get involved and make the right decision.
for (let i = 1; i < 1001; i++) {
    docs.push({unique1: i, b: i % 4, c: i % 3, d: i % 2, constant: 1, unique2: i});
};
coll.insert(docs);

for (const key of ["unique1", "b", "c", "d", "constant", "unique2"]) {
    coll.createIndex({[key]: 1});
    coll.runCommand({analyze: collName, key: key, numberBuckets: 1000});
}

// Additional compound (multi-field) indexes
coll.createIndex({unique1: 1, unique2: 1});
coll.createIndex({unique1: 1, unique2: 1, empty_field: 1});
coll.createIndex({unique1: 1, constant: 1});
coll.createIndex({unique2: 1, unique1: 1});
coll.createIndex({unique2: 1, unique1: 1, c: 1});
coll.createIndex({empty_field: 1, d: 1, b: 1});

function winningIndex(predicate) {
    const cursor = coll.find(predicate);
    return winningIndexFromCursor(cursor);
}

function winningIndexFromCursor(cursor) {
    const explain = cursor.explain();
    const winningPlan = getWinningPlanFromExplain(explain);

    // There can not be a true winning plan if there are no rejected alternatives
    assert.gt(getRejectedPlans(explain).length, 0);

    if (winningPlan.inputStage.indexName) {
        assert.eq(winningPlan.inputStage.stage, "IXSCAN");
        return winningPlan.inputStage.indexName;
    } else {
        assert.eq(winningPlan.inputStage.inputStage.stage, "IXSCAN");
        return winningPlan.inputStage.inputStage.indexName;
    }
}

try {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));

    /*
     * Plan selection for find()
     */

    // In the presence of a single-field predicate over a single-field index, it is always chosen
    // (as opposed to any compound (multi-field) indexes that have the same field).
    assert.eq(winningIndex({unique1: 1}), "unique1_1");
    assert.eq(winningIndex({unique1: {$gte: 0}}), "unique1_1");
    assert.eq(winningIndex({unique1: {$lt: 0}}), "unique1_1");

    // Same in the case of covering index scans.
    assert.eq(winningIndexFromCursor(coll.find({unique1: 1}, {_id: 0, unique1: 1})), "unique1_1");
    assert.eq(winningIndexFromCursor(coll.find({unique1: {$gte: 0}}, {_id: 0, unique1: 1})),
              "unique1_1");
    assert.eq(winningIndexFromCursor(coll.find({unique1: {$lt: 0}}, {_id: 0, unique1: 1})),
              "unique1_1");

    // In the presence of two predicates, the index on the more selective one is chosen.
    assert.eq(winningIndex({unique1: 1, b: 1}), "unique1_1");
    assert.eq(winningIndex({unique1: {$gte: 0}, b: 1}), "b_1");
    assert.eq(winningIndex({b: 1, c: 1}), "b_1");
    assert.eq(winningIndex({c: 1, d: 1}), "c_1");

    // In the presence of a lot of predicates, the index that is basically 'unique' (without it
    // being defined as such) is chosen (as opposed to any index intersection that could be
    // considered).
    assert.eq(winningIndex({unique1: 1, b: 1, c: 1, d: 1}), "unique1_1");
    assert.eq(winningIndex({unique1: -1, b: -1, c: -1, d: -1}), "unique1_1");

    // In the presence of a predicate that has no matching rows, its index is chosen.
    assert.eq(winningIndex({unique1: -1, b: 1}), "unique1_1");
    assert.eq(winningIndex({b: -1, c: 1}), "b_1");
    assert.eq(winningIndex({c: -1, d: 1}), "c_1");

    assert.eq(winningIndex({unique1: -1, b: 1}), "unique1_1");
    assert.eq(winningIndex({unique1: -1, b: 1, c: 1}), "unique1_1");
    assert.eq(winningIndex({unique1: -1, b: 1, c: 1, d: 1}), "unique1_1");

    // In the presence of a predicate that is not selective at all, its index is never chosen.
    assert.neq(winningIndex({unique1: {$gte: 0}, b: 1}), "unique1_1");
    assert.neq(winningIndex({unique1: {$gte: 0}, b: 1, c: 1}), "unique1_1");
    assert.neq(winningIndex({unique1: {$gte: 0}, b: {$gte: 0}, c: 1, d: 1}), "unique1_1");
    assert.neq(winningIndex({unique1: {$gte: 0}, b: {$gte: 0}, c: 1, d: 1}), "unique1_1");
    assert.eq(winningIndex({unique1: {$gte: 0}, b: {$gte: 0}, c: {$gte: 0}, d: 1}), "d_1");

    /*
     * Plan selection for find() + compound (multi-field) index
     */

    // In the presence of a compound index, it is chosen regardless of the selectivity of the
    // predicates.
    assert.contains(winningIndex({unique1: 1, unique2: 1}),
                    ["unique1_1_unique2_1", "unique2_1_unique1_1"]);
    assert.eq(winningIndex({unique1: {$gte: 0}, unique2: {$gte: 0}}), "unique1_1_unique2_1");
    assert.eq(winningIndex({unique1: 1, unique2: {$gte: 0}}), "unique1_1_unique2_1");

    // TODO(SERVER-98102): assert.eq(winningIndex({unique1: {$lt: 0}, unique2: {$lt: 0}}),
    // "unique1_1_unique2_1");
    // TODO(SERVER-98102): assert.eq(winningIndex({unique1: 1, unique2: {$lt: 0}}),
    // "unique1_1_unique2_1");

    assert.eq(winningIndex({unique1: 1, constant: 1}), "unique1_1_constant_1");
    assert.eq(winningIndex({unique1: 1, constant: {$gte: 0}}), "unique1_1_constant_1");

    assert.eq(winningIndex({unique1: {$gte: 0}, constant: {$gte: 0}}), "unique1_1_constant_1");
    // TODO(SERVER-98102): assert.eq(winningIndex({unique1: {$lt: 0}, constant: {$gte: 0}}),
    // "unique1_1_constant_1");

    // Always choose the index that puts the more selective predicate first
    // TODO(SERVER-98102): assert.eq(winningIndex({unique1: 1, unique2: {$lt: 0}}),
    // "unique1_1_unique2_1");
    assert.eq(winningIndex({unique1: {$gte: 500}, unique2: 1}), "unique2_1_unique1_1");
    assert.eq(winningIndex({unique1: {$gte: 500}, unique2: {$in: [1, 2, 3]}}),
              "unique2_1_unique1_1");
    assert.eq(winningIndex({unique1: 1, unique2: {$gte: 500}}), "unique1_1_unique2_1");
    // TODO(SERVER-98102): assert.eq(winningIndex({unique1: {$lt: 0}, unique2: 1}),
    // "unique2_1_unique1_1");
    assert.eq(winningIndex({unique1: {$gt: 500}, unique2: {$gt: 700}}), "unique2_1_unique1_1");

    /*
     * Plan selection for find() + sort() on the same field.
     */

    // If there is NO clear selectivity winner among the other predicates,
    // we pick the index of the sort.
    assert.eq(winningIndexFromCursor(coll.find({b: 1, c: 1}).sort({b: 1})), "b_1");
    assert.eq(winningIndexFromCursor(coll.find({b: 1, c: 1, d: 1}).sort({b: 1})), "b_1");
    assert.eq(winningIndexFromCursor(coll.find({b: 1, c: 1, d: 1}).sort({c: 1})), "c_1");

    // Sort on the shortest index that can be brought to bear to do the find() and the sort()
    assert.eq(winningIndexFromCursor(coll.find({unique1: 1, unique2: 1}).sort({unique1: 1})),
              "unique1_1_unique2_1");
    assert.eq(winningIndexFromCursor(coll.find({unique1: 1, unique2: -1}).sort({unique1: 1})),
              "unique1_1_unique2_1");
    assert.eq(
        winningIndexFromCursor(coll.find({unique1: 1, unique2: {$gte: 0}}).sort({unique1: 1})),
        "unique1_1_unique2_1");
    assert.eq(winningIndexFromCursor(coll.find({unique1: 1, b: 1}).sort({unique1: 1})),
              "unique1_1");

    // If there IS a clear selectivity winner among the other predicates,
    // we pick the index of that predicate.
    assert.eq(winningIndexFromCursor(coll.find({unique1: 1, b: 1, c: 1, d: 1}).sort({b: 1})),
              "unique1_1");

    /*
     * Plan selection for find() + sort() on different fields.
     */

    // In the presence of a very-selective find() + sort(), we pick the index on the find()
    assert.eq(winningIndexFromCursor(coll.find({unique1: -1}).sort({b: 1})), "unique1_1");
    assert.eq(winningIndexFromCursor(coll.find({b: -1}).sort({unique1: 1})), "b_1");
    assert.eq(winningIndexFromCursor(coll.find({unique1: 1}).sort({b: 1})), "unique1_1");

    // TODO(SERVER-100647):
    // assert.eq(winningIndexFromCursor(coll.find({a:-1}).sort({b:1}).limit(1)), "unique1_1");
    // TODO(SERVER-100647):
    // assert.eq(winningIndexFromCursor(coll.find({b:-1}).sort({a:1}).limit(1)), "b_1");
    // TODO(SERVER-100647): assert.eq(winningIndexFromCursor(coll.find({a:1}).sort({b:1}).limit(1)),
    // "unique1_1");

    // In the presence of non-selective find() + sort(), we pick the index on the sort
    assert.eq(winningIndexFromCursor(coll.find({unique1: {$gte: 0}}).sort({b: 1})), "b_1");
    assert.eq(winningIndexFromCursor(coll.find({b: {$gte: 0}}).sort({unique1: 1})), "unique1_1");

    // In the presence of sort() + limit(1) we pick the index on the predicate.
    assert.eq(winningIndexFromCursor(coll.find({unique1: {$gte: 0}}).sort({b: 1}).limit(1)),
              "unique1_1");
    assert.eq(winningIndexFromCursor(coll.find({b: {$gte: 0}}).sort({unique1: 1}).limit(1)), "b_1");

    // In the presence of sort() + large limit, we pick again the index on the sort field.
    assert.eq(winningIndexFromCursor(coll.find({unique1: {$gte: 0}}).sort({b: 1}).limit(10000)),
              "b_1");
    assert.eq(winningIndexFromCursor(coll.find({b: {$gte: 0}}).sort({unique1: 1}).limit(10000)),
              "unique1_1");
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
