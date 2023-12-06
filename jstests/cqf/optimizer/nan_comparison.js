/**
 * Tests comparisons against NaN.
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    getPlanSkeleton,
    runWithFastPathsDisabled,
    usedBonsaiOptimizer
} from "jstests/libs/optimizer_utils.js";

const coll = db.nan_comparison;
coll.drop();

function runFindAssertBonsai(filter, expectedResult) {
    const result = coll.find(filter).toArray();
    assertArrayEq({actual: result, expected: expectedResult});

    // Assert Bonsai was used.
    const explain =
        assert.commandWorked(db.runCommand({explain: {find: coll.getName(), filter: filter}}));
    assert(usedBonsaiOptimizer(explain), tojson(explain));
}

const docs = [{_id: 0, a: NaN}, {_id: 1, a: 5}];
coll.insert(docs);
assert.commandWorked(coll.createIndex({a: 1}));

// Comparisons to non-NaN values should not match NaN values.
runFindAssertBonsai({a: {$lt: 0}}, []);
runFindAssertBonsai({a: {$lte: 0}}, []);
runFindAssertBonsai({a: {$lt: -Infinity}}, []);
runFindAssertBonsai({a: {$lte: -Infinity}}, []);

// Queries which should be simplified to equalities to NaN and only match NaN values.
function assertMatchOnlyNaN(filter) {
    jsTestLog('Testing...' + tojson(filter));
    const result = coll.find(filter).toArray();
    assertArrayEq({actual: result, expected: [docs[0]], extraErrorMsg: tojson(filter)});

    const expectedExplain = {
        "nodeType": "Root",
        "child": {
            "nodeType": "NestedLoopJoin",
            "leftChild":
                {"nodeType": "IndexScan", "indexDefName": "a_1", "interval": "[ NaN, NaN ]"},
            "rightChild": {"nodeType": "LimitSkip", "child": {"nodeType": "Seek"}}
        }
    };

    const explain = coll.explain().find(filter).hint({a: 1}).finish();
    const skeleton = getPlanSkeleton(explain.queryPlanner.winningPlan.queryPlan, {
        extraKeepKeys: ['indexDefName', 'interval'],
    });
    assert.eq(skeleton, expectedExplain, {filter: filter, explain: explain, skeleton: skeleton});
}
assertMatchOnlyNaN({a: {$eq: NaN}});
assertMatchOnlyNaN({a: {$lte: NaN}});
assertMatchOnlyNaN({a: {$gte: NaN}});
assertMatchOnlyNaN({a: {$lte: NaN, $gte: NaN}});
assertMatchOnlyNaN({$and: [{a: {$lte: NaN}}, {a: {$gte: NaN}}]});
assertMatchOnlyNaN({$or: [{a: {$lte: NaN}}, {a: {$gte: NaN}}]});
assertMatchOnlyNaN({$or: [{a: {$lt: NaN}}, {a: {$gte: NaN}}]});
assertMatchOnlyNaN({$or: [{a: {$lte: NaN}}, {a: {$gt: NaN}}]});

// Comparisons to NaN which should be simplified to contradictions.
function assertEmptyCoScan(filter) {
    jsTestLog('Testing...' + tojson(filter));
    const result = coll.find(filter).toArray();
    assertArrayEq({actual: result, expected: [], extraErrorMsg: tojson(filter)});

    const expectedExplain = {
        "nodeType": "Root",
        "child": {
            "nodeType": "Evaluation",
            "child": {"nodeType": "LimitSkip", "child": {"nodeType": "CoScan"}}
        }
    };

    const explain = runWithFastPathsDisabled(() => coll.explain().find(filter).finish());
    const skeleton = getPlanSkeleton(explain.queryPlanner.winningPlan.queryPlan);
    assert.eq(skeleton, expectedExplain, {filter: filter, explain: explain, skeleton: skeleton});
}

assertEmptyCoScan({a: {$lt: NaN}});
assertEmptyCoScan({a: {$gt: NaN}});
assertEmptyCoScan({a: {$lt: NaN, $gte: NaN}});
assertEmptyCoScan({a: {$lte: NaN, $gt: NaN}});
assertEmptyCoScan({a: {$lt: NaN, $gt: NaN}});
assertEmptyCoScan({a: {$eq: NaN, $lt: NaN}});
assertEmptyCoScan({$and: [{a: {$lt: NaN}}, {a: {$gte: NaN}}]});
assertEmptyCoScan({$or: [{a: {$lt: NaN}}, {a: {$gt: NaN}}]});
