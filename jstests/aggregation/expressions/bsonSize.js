/**
 * @tags: [
 *   sbe_incompatible,
 * ]
 */
(function() {
"use strict";

const coll = db.agg_expr_bsonSize;
coll.drop();
assert.commandWorked(coll.insert({_id: 1}));

function checkBsonSize() {
    assert.eq(Object.bsonsize(coll.findOne()),
              coll.aggregate([{$project: {s: {$bsonSize: "$$CURRENT"}}}]).next().s);
}

checkBsonSize();

assert.commandWorked(coll.update({_id: 1}, {$push: {xs: 1}}));
checkBsonSize();

assert.commandWorked(coll.update({_id: 1}, {$push: {xs: {subdoc: 12345}}}));
checkBsonSize();

assert.commandWorked(coll.update({_id: 1}, {$push: {xs: 'x'.repeat(7)}}));
checkBsonSize();

assert.commandWorked(coll.update({_id: 1}, {$push: {xs: 'x'.repeat(500)}}));
checkBsonSize();

assert.commandWorked(coll.update({_id: 1}, {$push: {xs: 'x'.repeat(16 * 1e6)}}));
checkBsonSize();

// embedded arrays
assert.commandWorked(coll.update({_id: 1}, {$set: {arr: [1, 2, 3, 4]}}));
checkBsonSize();

// subdocuments
assert.commandWorked(coll.update({_id: 1}, {$set: {arr: {a: {b: {c: 1}}}}}));
checkBsonSize();

// bsonSize's argument must be a document
function checkExpectsDocument(badInput) {
    assert.throws(() => coll.aggregate([{$project: {x: {$bsonSize: {$literal: badInput}}}}]),
                  [],
                  "$bsonSize requires a document input");
}
checkExpectsDocument(123);
checkExpectsDocument("abc");
checkExpectsDocument(BinData(0, "aaaa"));
checkExpectsDocument([123, 456]);
checkExpectsDocument([{x: 1}, {y: 2}]);
}());
