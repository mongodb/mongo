/**
 * Basic test coverage for the $anyElementTrue expression.
 */
(function() {
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.
const coll = db.any_element_true;
coll.drop();
assert.commandWorked(coll.insert({
    _id: 0,
    allTrue: [true, true],
    someTrue: [true, false],
    noneTrue: [0, false],
    nonArray: 1,
    nullInput: [null],
    undefinedInput: [undefined],
    undefinedTrue: [undefined, true],
    nullTrue: [null, true],
    empty: []
}));

function testOp(expression, expected) {
    const results = coll.aggregate([{$project: {_id: 0, result: expression}}]).toArray();
    assert.eq(results.length, 1, results);
    const loneResult = results[0];
    assert(loneResult.hasOwnProperty("result"));
    assert.eq(loneResult.result, expected, loneResult);
}

function assertThrows(expression) {
    const error =
        assert.throws(() => coll.aggregate([{$project: {_id: 0, result: expression}}]).toArray());
    assert.commandFailedWithCode(error, 5159200);
}

testOp({$anyElementTrue: {$literal: [true, true]}}, true);
testOp({$anyElementTrue: {$literal: [1, true]}}, true);
testOp({$anyElementTrue: {$literal: [true, 0]}}, true);
testOp({$anyElementTrue: {$literal: [true, 1, false]}}, true);
testOp({$anyElementTrue: {$literal: [false, 0, false]}}, false);
testOp({$anyElementTrue: "$allTrue"}, true);
testOp({$anyElementTrue: "$someTrue"}, true);
testOp({$anyElementTrue: "$noneTrue"}, false);
testOp({$anyElementTrue: ["$noneTrue"]}, false);
testOp({$anyElementTrue: [["$non_existent_field"]]}, false);
testOp({$anyElementTrue: [["$non_existent_field", true]]}, true);
testOp({$anyElementTrue: "$nullInput"}, false);
testOp({$anyElementTrue: "$undefinedInput"}, false);
testOp({$anyElementTrue: "$undefinedTrue"}, true);
testOp({$anyElementTrue: "$nullTrue"}, true);
testOp({$anyElementTrue: "$empty"}, false);
assertThrows({$anyElementTrue: "$nonArray"});
assertThrows({$anyElementTrue: ["$non_existent_field"]});
}());
