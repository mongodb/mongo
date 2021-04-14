/**
 * Basic test coverage for the $allElementsTrue expression.
 */
(function() {
const coll = db.all_elements_true;
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

testOp({$allElementsTrue: {$literal: [true, true]}}, true);
testOp({$allElementsTrue: {$literal: [1, true]}}, true);
testOp({$allElementsTrue: {$literal: [true, 0]}}, false);
testOp({$allElementsTrue: {$literal: [true, 1, false]}}, false);
testOp({$allElementsTrue: "$allTrue"}, true);
testOp({$allElementsTrue: "$someTrue"}, false);
testOp({$allElementsTrue: "$noneTrue"}, false);
}());
