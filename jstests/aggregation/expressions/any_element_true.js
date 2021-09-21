/**
 * Basic test coverage for the $anyElementTrue expression.
 */
(function() {
const coll = db.any_element_true;
coll.drop();
assert.commandWorked(
    coll.insert({_id: 0, allTrue: [true, true], someTrue: [true, false], noneTrue: [0, false]}));

function testOp(expression, expected) {
    const results = coll.aggregate([{$project: {_id: 0, result: expression}}]).toArray();
    assert.eq(results.length, 1, results);
    const loneResult = results[0];
    assert(loneResult.hasOwnProperty("result"));
    assert.eq(loneResult.result, expected, loneResult);
}

testOp({$anyElementTrue: {$literal: [true, true]}}, true);
testOp({$anyElementTrue: {$literal: [1, true]}}, true);
testOp({$anyElementTrue: {$literal: [true, 0]}}, true);
testOp({$anyElementTrue: {$literal: [true, 1, false]}}, true);
testOp({$anyElementTrue: {$literal: [false, 0, false]}}, false);
testOp({$anyElementTrue: "$allTrue"}, true);
testOp({$anyElementTrue: "$someTrue"}, true);
testOp({$anyElementTrue: "$noneTrue"}, false);
}());
