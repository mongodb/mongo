/**
 * Tests the 'n' family of accumulators implemented as expressions.
 * TODO SERVER-57881: Add testcases for $firstN/$lastN.
 */
(function() {
'use strict';

const coll = db[jsTestName()];
const doc = {
    a: [1, 2, 3, 5, 7, 9],
    n: 4,
    diff: 2
};

const isExactTopNEnabled = db.adminCommand({getParameter: 1, featureFlagExactTopNAccumulator: 1})
                               .featureFlagExactTopNAccumulator.value;

if (!isExactTopNEnabled) {
    // Verify that $minN/$maxN cannot be used if the feature flag is set to false and ignore the
    // rest of the test.
    assert.commandFailedWithCode(coll.runCommand("aggregate", {
        pipeline: [{$project: {output: {'$minN': {n: 3, output: [3, 1, 2, 3]}}}}],
        cursor: {}
    }),
                                 5787909);
    return;
}

coll.drop();
assert.commandWorked(coll.insert(doc));

function testExpr(expression, expected) {
    assert.eq(coll.aggregate([{$project: {_id: 0, output: expression}}]).toArray()[0].output,
              expected);
}

let args = {n: 3, output: [5, 4, 3, 2, 1]};
testExpr({$minN: args}, [1, 2, 3]);
testExpr({$maxN: args}, [5, 4, 3]);
args = {
    n: 3,
    output: [null, 2, null, 1]
};
testExpr({$minN: args}, [1, 2]);
testExpr({$maxN: args}, [2, 1]);
args = {
    n: 3,
    output: "$a"
};
testExpr({$minN: args}, [1, 2, 3]);
testExpr({$maxN: args}, [9, 7, 5]);
args = {
    n: "$n",
    output: "$a"
};
testExpr({$minN: args}, [1, 2, 3, 5]);
testExpr({$maxN: args}, [9, 7, 5, 3]);
args = {
    n: {$subtract: ["$n", "$diff"]},
    output: [3, 4, 5]
};
testExpr({$minN: args}, [3, 4]);
testExpr({$maxN: args}, [5, 4]);
})();