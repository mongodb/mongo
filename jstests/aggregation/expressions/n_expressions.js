/**
 * Tests the 'n' family of accumulators implemented as expressions.
 */
(function() {
'use strict';

const coll = db[jsTestName()];
const doc = {
    a: [1, 2, 3, 5, 7, 9],
    n: 4,
    diff: 2
};

coll.drop();
assert.commandWorked(coll.insert(doc));

function testExpr(expression, expected) {
    assert.eq(coll.aggregate([{$project: {_id: 0, output: expression}}]).toArray()[0].output,
              expected);
}

let args = {n: 3, input: [5, 4, 3, 2, 1]};
testExpr({$minN: args}, [1, 2, 3]);
testExpr({$maxN: args}, [5, 4, 3]);
testExpr({$firstN: args}, [5, 4, 3]);
testExpr({$lastN: args}, [3, 2, 1]);
args = {
    n: 3,
    input: [null, 2, null, 1]
};
testExpr({$minN: args}, [1, 2]);
testExpr({$maxN: args}, [2, 1]);
testExpr({$firstN: args}, [null, 2, null]);
testExpr({$lastN: args}, [2, null, 1]);
args = {
    n: 3,
    input: "$a"
};
testExpr({$minN: args}, [1, 2, 3]);
testExpr({$maxN: args}, [9, 7, 5]);
testExpr({$firstN: args}, [1, 2, 3]);
testExpr({$lastN: args}, [5, 7, 9]);
args = {
    n: "$n",
    input: "$a"
};
testExpr({$minN: args}, [1, 2, 3, 5]);
testExpr({$maxN: args}, [9, 7, 5, 3]);
testExpr({$firstN: args}, [1, 2, 3, 5]);
testExpr({$lastN: args}, [3, 5, 7, 9]);
args = {
    n: {$subtract: ["$n", "$diff"]},
    input: [3, 4, 5]
};
testExpr({$minN: args}, [3, 4]);
testExpr({$maxN: args}, [5, 4]);
testExpr({$firstN: args}, [3, 4]);
testExpr({$lastN: args}, [4, 5]);
})();
