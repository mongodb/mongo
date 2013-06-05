// SERVER-9841 $map expression to map over arrays

load('jstests/aggregation/extras/utils.js');
var t = db.server9841;
t.drop();
t.insert({
    simple: [1,2,3,4],
    nested: [{a: 1}, {a: 2}],
    mixed: [{a: 1}, {}, {a: 2}, {a: null}],
    notArray: 1,
    null: null,
});

function test(expression, expected) {
    var result = t.aggregate({$project:{_id:0, res: expression}}).result;
    assert.eq(result, [{res:expected}]);
}

test({$map: {input: "$simple", as: "var", in: '$$var'}}, [1,2,3,4]);
test({$map: {input: "$simple", as: "var", in: {$add:[10, '$$var']}}}, [11,12,13,14]);

test({$map: {input: "$nested", as: "var", in: '$$var.a'}}, [1,2]);
test({$map: {input: "$nested", as: "CURRENT", in: '$a'}}, [1,2]);

test({$map: {input: "$mixed", as: "var", in: '$$var.a'}}, [1,null,2,null]); // missing becomes null
             
test({$map: {input: "$null", as: "var", in: '$$var'}}, null);

// can't set ROOT
assertErrorCode(t, {$project:{a:{$map:{input: "$simple", as: "ROOT", in: '$$ROOT'}}}}, 16867);

// error on non-array
assertErrorCode(t, {$project:{a:{$map:{input: "$notArray", as: "var", in: '$$var'}}}}, 16883);

// parse errors (missing or extra fields)
assertErrorCode(t, {$project:{a:{$map:{x:1, input: "$simple", as: "var", in: '$$var'}}}}, 16879);
assertErrorCode(t, {$project:{a:{$map:{as: "var", in: '$$var'}}}}, 16880);
assertErrorCode(t, {$project:{a:{$map:{input: "$simple", in: '$$var'}}}}, 16881);
assertErrorCode(t, {$project:{a:{$map:{input: "$simple", as: "var"}}}}, 16882);

