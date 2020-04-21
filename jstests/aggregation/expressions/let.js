/**
 * Basic integration tests for the $let expression.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

let coll = db.agg_expr_let;
coll.drop();
assert.commandWorked(coll.insert({zero: 0, one: 1, two: 2, three: 3, nested: {four: 4}}));

function testExpr(expression, output) {
    const res = coll.aggregate([{$project: {output: expression}}]).toArray();
    assert.eq(res.length, 1, tojson(res));
    assert.eq(res[0].output, output, tojson(res));

    // Test in group:
    const result = coll.aggregate({$group: {_id: 0, res: {$sum: expression}}}).toArray();
    assert.eq(result, [{_id: 0, res: output}]);
}

// Basic tests.
testExpr('$two', 2);
testExpr('$$CURRENT.two', 2);
testExpr('$$ROOT.two', 2);

// Using sub expressions.
testExpr({$add: ['$two', '$$CURRENT.three']}, 5);
testExpr({$add: ['$$CURRENT.two', '$$ROOT.nested.four']}, 6);

// Verify that the variables defined in $let work.
testExpr({$let: {vars: {a: 10}, in : '$$a'}}, 10);
testExpr({$let: {vars: {a: '$zero'}, in : '$$a'}}, 0);
testExpr({$let: {vars: {a: {$add: ['$one', '$two']}, b: 10}, in : {$multiply: ['$$a', '$$b']}}},
         30);

// Verify that the outer level variable works in inner level $let.
testExpr({
    $let:
        {vars: {var1: 1}, in : {$let: {vars: {var2: "$$var1"}, in : {$sum: ["$$var1", "$$var2"]}}}}
},
         2);

// Verify that the outer level variables get overwritten by inner level variables.
testExpr({
        $let: {
            vars: {var1: "$one"},
            in : {$let: {vars: {var2: "$$var1", var1: 3}, in : {$sum: ["$$var2", "$$var1"]}}}
        }
    },
             4);

// $let changing CURRENT
testExpr({$let: {vars: {CURRENT: '$$ROOT.nested'}, in : {$multiply: ['$four', '$$ROOT.two']}}}, 8);
testExpr({
    $let: {
        vars: {CURRENT: '$$CURRENT.nested'},  // using original value of CURRENT
        in : {$multiply: ['$four', '$$ROOT.two']}
    }
},
         8);
testExpr({
    $let: {
        vars: {CURRENT: '$nested'},  // same as last
        in : {$multiply: ['$four', '$$ROOT.two']}
    }
},
         8);
testExpr({
    $let: {
        vars: {CURRENT: {$const: {ten: 10}}},  // "artificial" object
        in : {$multiply: ['$ten', '$$ROOT.two']}
    }
},
         20);
testExpr({
    $let: {
        vars: {CURRENT: '$three'},  // sets current to the number 3 (not an object)
        in : {$multiply: ['$$CURRENT', '$$ROOT.two']}
    }
},
         6);

// Swapping with $let (ensures there is no ordering dependency in vars).
testExpr({
        $let: {
            vars: {x: 6, y: 10},
            in : {
                $let: {
                    vars: {x: '$$y', y: '$$x'},  // now {x:10, y:6}
                    in : {$subtract: ['$$x', '$$y']}
                }
            }
        }
    },            // Not commutative!
             4);  // 10-6 not 6-10 or 6-6

// Unicode is allowed.
testExpr({$let: {vars: {'日本語': 10}, in : '$$日本語'}},
         10);  // Japanese for "Japanese language".

// Can use ROOT and CURRENT directly with no subfield (SERVER-5916).
coll.drop();
coll.insert({_id: 'obj'});
assert.eq(coll.aggregate({$project: {_id: 0, obj: '$$ROOT'}}).toArray(), [{obj: {_id: 'obj'}}]);
assert.eq(coll.aggregate({$project: {_id: 0, obj: '$$CURRENT'}}).toArray(), [{obj: {_id: 'obj'}}]);
assert.eq(coll.aggregate({$group: {_id: 0, objs: {$push: '$$ROOT'}}}).toArray(),
          [{_id: 0, objs: [{_id: 'obj'}]}]);
assert.eq(coll.aggregate({$group: {_id: 0, objs: {$push: '$$CURRENT'}}}).toArray(),
          [{_id: 0, objs: [{_id: 'obj'}]}]);

// Check name validity checks.
assertErrorCode(coll, {$project: {a: {$let: {vars: {ROOT: 1}, in : '$$ROOT'}}}}, 16867);
assertErrorCode(coll, {$project: {a: {$let: {vars: {FOO: 1}, in : '$$FOO'}}}}, 16867);
assertErrorCode(coll, {$project: {a: {$let: {vars: {_underbar: 1}, in : '$$FOO'}}}}, 16867);
assertErrorCode(coll, {$project: {a: {$let: {vars: {'a.b': 1}, in : '$$FOO'}}}}, 16868);
assertErrorCode(coll, {$project: {a: {$let: {vars: {'a b': 1}, in : '$$FOO'}}}}, 16868);
assertErrorCode(coll, {$project: {a: '$$_underbar'}}, 16867);
assertErrorCode(coll, {$project: {a: '$$with spaces'}}, 16868);

// Verify that variables defined in '$let' cannot be used to initialize other variables.
assertErrorCode(
    coll,
    [{$project: {output: {$let: {vars: {var1: "$one", var2: "$$var1"}, in : "$$var1"}}}}],
    17276);
}());
