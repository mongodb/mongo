// Tests for $array expression.
(function() {
"use strict";

let coll = db.array_expr;
coll.drop();

function assertArray(expArray, ...inputs) {
    assert(coll.drop());
    if (inputs.length == 0) {
        assert.commandWorked(coll.insert({}));
    } else if (inputs.length == 1) {
        assert.commandWorked(coll.insert({a: inputs[0]}));
    } else {
        assert.commandWorked(coll.insert({a: inputs[0], b: inputs[1]}));
    }
    const result = coll.aggregate([{$project: {out: ["$a", "$b"]}}]).toArray()[0].out;
    assert.eq(result, expArray);
}

assertArray([1, 2], 1, 2);
assertArray([null, null], null, null);
assertArray(["TestInput", null], "TestInput", null);
assertArray([{a: 1, b: 2}, [1, 2]], {a: 1, b: 2}, [1, 2]);
assertArray(["TestInput", null], "TestInput");
assertArray([null, null]);

// no arg
assert(coll.drop());
assert.commandWorked(coll.insert({}));
let result = coll.aggregate([{$project: {out: []}}]).toArray()[0].out;
assert.eq(result, []);
}());
