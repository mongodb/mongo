// SERVER-9840 variables in expressions and $let

load('jstests/aggregation/extras/utils.js');
var t = db.server9840;
t.drop();

function test(expression, expected) {
    t.drop();
    t.insert({zero: 0, one: 1, two: 2, three: 3, nested: {four: 4}});

    // Test in projection:
    var result = t.aggregate({$project: {_id: 0, res: expression}}).toArray();
    assert.eq(result, [{res: expected}]);

    // Test in group:
    var result = t.aggregate({$group: {_id: 0, res: {$sum: expression}}}).toArray();
    assert.eq(result, [{_id: 0, res: expected}]);
}

// basics
test('$two', 2);
test('$$CURRENT.two', 2);
test('$$ROOT.two', 2);

// using sub expressions
test({$add: ['$two', '$$CURRENT.three']}, 5);
test({$add: ['$$CURRENT.two', '$$ROOT.nested.four']}, 6);

// $let simple
test({$let: {vars: {a: 10}, in : '$$a'}}, 10);
test({$let: {vars: {a: '$zero'}, in : '$$a'}}, 0);
test({$let: {vars: {a: {$add: ['$one', '$two']}, b: 10}, in : {$multiply: ['$$a', '$$b']}}}, 30);

// $let changing CURRENT
test({$let: {vars: {CURRENT: '$$ROOT.nested'}, in : {$multiply: ['$four', '$$ROOT.two']}}}, 8);
test({
    $let: {
        vars: {CURRENT: '$$CURRENT.nested'},  // using original value of CURRENT
        in : {$multiply: ['$four', '$$ROOT.two']}
    }
},
     8);
test({
    $let: {
        vars: {CURRENT: '$nested'},  // same as last
        in : {$multiply: ['$four', '$$ROOT.two']}
    }
},
     8);
test({
    $let: {
        vars: {CURRENT: {$const: {ten: 10}}},  // "artificial" object
        in : {$multiply: ['$ten', '$$ROOT.two']}
    }
},
     20);
test({
    $let: {
        vars: {CURRENT: '$three'},  // sets current to the number 3 (not an object)
        in : {$multiply: ['$$CURRENT', '$$ROOT.two']}
    }
},
     6);

// swapping with $let (ensures there is no ordering dependency in vars)
test({
    $let: {
        vars: {x: 6, y: 10},
        in : {
            $let: {
                vars: {x: '$$y', y: '$$x'},  // now {x:10, y:6}
                in : {$subtract: ['$$x', '$$y']}
            }
        }
    }
},        // not commutative!
     4);  // 10-6 not 6-10 or 6-6

// unicode is allowed
test({$let: {vars: {'日本語': 10}, in : '$$日本語'}}, 10);  // Japanese for "Japanese language"

// Can use ROOT and CURRENT directly with no subfield (SERVER-5916)
t.drop();
t.insert({_id: 'obj'});
assert.eq(t.aggregate({$project: {_id: 0, obj: '$$ROOT'}}).toArray(), [{obj: {_id: 'obj'}}]);
assert.eq(t.aggregate({$project: {_id: 0, obj: '$$CURRENT'}}).toArray(), [{obj: {_id: 'obj'}}]);
assert.eq(t.aggregate({$group: {_id: 0, objs: {$push: '$$ROOT'}}}).toArray(),
          [{_id: 0, objs: [{_id: 'obj'}]}]);
assert.eq(t.aggregate({$group: {_id: 0, objs: {$push: '$$CURRENT'}}}).toArray(),
          [{_id: 0, objs: [{_id: 'obj'}]}]);

// check name validity checks
assertErrorCode(t, {$project: {a: {$let: {vars: {ROOT: 1}, in : '$$ROOT'}}}}, 16867);
assertErrorCode(t, {$project: {a: {$let: {vars: {FOO: 1}, in : '$$FOO'}}}}, 16867);
assertErrorCode(t, {$project: {a: {$let: {vars: {_underbar: 1}, in : '$$FOO'}}}}, 16867);
assertErrorCode(t, {$project: {a: {$let: {vars: {'a.b': 1}, in : '$$FOO'}}}}, 16868);
assertErrorCode(t, {$project: {a: {$let: {vars: {'a b': 1}, in : '$$FOO'}}}}, 16868);
assertErrorCode(t, {$project: {a: '$$_underbar'}}, 16870);
assertErrorCode(t, {$project: {a: '$$with spaces'}}, 16871);
