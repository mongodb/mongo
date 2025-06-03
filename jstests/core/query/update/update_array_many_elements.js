// Tests on arrays with >10 elements to ensure that array elements are not accidentally reordered
// due to lexicographic field order.
//
// The tests perform several simple updates with several array-related functions.
//
// See SERVER-91432 for more information.

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// create an array with >10 elements; use NumberInt to allow $bit operator later
function resetDb() {
    let coll = db[jsTestName()];
    coll.drop();
    assert.commandWorked(coll.insert({
        _id: 1,
        scores: [
            NumberInt(0),
            NumberInt(1),
            NumberInt(2),
            NumberInt(3),
            NumberInt(4),
            NumberInt(5),
            NumberInt(6),
            NumberInt(7),
            NumberInt(8),
            NumberInt(9),
            NumberInt(10),
            NumberInt(11),
            NumberInt(12),
            NumberInt(13),
            NumberInt(14),
            NumberInt(15)
        ]
    }));
    return coll;
}

let coll;

///////////////////////////////////////////////////////////////////////////////////////////////////
// pop
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1}, {$pop: {scores: -1}}));
assert.docEq(coll.findOne(), {_id: 1, scores: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]});

coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1}, {$pop: {scores: 1}}));
assert.docEq(coll.findOne(), {_id: 1, scores: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14]});

///////////////////////////////////////////////////////////////////////////////////////////////////
// pull
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1}, {$pull: {scores: {$gte: 5, $lte: 10}}}));
assert.docEq(coll.findOne(), {_id: 1, scores: [0, 1, 2, 3, 4, 11, 12, 13, 14, 15]});

///////////////////////////////////////////////////////////////////////////////////////////////////
// pullAll
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1}, {$pullAll: {scores: [1, 2, 3, 4, 5]}}));
assert.docEq(coll.findOne(), {_id: 1, scores: [0, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]});

///////////////////////////////////////////////////////////////////////////////////////////////////
// push
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1}, {$push: {scores: 16}}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]});

coll = resetDb();
assert.commandWorked(
    coll.updateOne({_id: 1}, {$push: {scores: {$each: [16, 17, 18], $position: 10, $slice: -10}}}));
assert.docEq(coll.findOne(), {_id: 1, scores: [9, 16, 17, 18, 10, 11, 12, 13, 14, 15]});

///////////////////////////////////////////////////////////////////////////////////////////////////
// addToSet
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1}, {$addToSet: {scores: 10}}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]});

coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1}, {$addToSet: {scores: {$each: [14, 15, 16]}}}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]});

///////////////////////////////////////////////////////////////////////////////////////////////////
// set
///////////////////////////////////////////////////////////////////////////////////////////////////

// dot notation
coll = resetDb();
assert.commandWorked(
    coll.updateOne({_id: 1}, {$set: {"scores.1": 10, "scores.10": 100, "scores.13": 130}}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, 10, 2, 3, 4, 5, 6, 7, 8, 9, 100, 11, 12, 130, 14, 15]});

// $[<identifier>]
coll = resetDb();
assert.commandWorked(coll.updateOne(
    {_id: 1}, {$set: {"scores.$[element]": 99}}, {arrayFilters: [{element: {$gte: 9}}]}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, 1, 2, 3, 4, 5, 6, 7, 8, 99, 99, 99, 99, 99, 99, 99]});

// $[]
coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1}, {$set: {"scores.$[]": 99}}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99]});

// $
coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1, scores: 10}, {$set: {"scores.$": 100}}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 100, 11, 12, 13, 14, 15]});

///////////////////////////////////////////////////////////////////////////////////////////////////
// inc
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(
    coll.updateOne({_id: 1}, {$inc: {"scores.1": 10, "scores.10": 100, "scores.12": 1000}}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, 11, 2, 3, 4, 5, 6, 7, 8, 9, 110, 11, 1012, 13, 14, 15]});

///////////////////////////////////////////////////////////////////////////////////////////////////
// rename
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1}, {$rename: {"scores": "array"}}));
assert.docEq(coll.findOne(),
             {_id: 1, array: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]});

///////////////////////////////////////////////////////////////////////////////////////////////////
// max
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(
    coll.updateOne({_id: 1}, {$max: {"scores.1": 10, "scores.10": 10, "scores.12": 100}}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, 10, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 100, 13, 14, 15]});

///////////////////////////////////////////////////////////////////////////////////////////////////
// min
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(
    coll.updateOne({_id: 1}, {$min: {"scores.1": 10, "scores.10": 10, "scores.12": 10}}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 10, 13, 14, 15]});

///////////////////////////////////////////////////////////////////////////////////////////////////
// mul
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1}, {$mul: {"scores.1": 10, "scores.10": 100}}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, 10, 2, 3, 4, 5, 6, 7, 8, 9, 1000, 11, 12, 13, 14, 15]});

///////////////////////////////////////////////////////////////////////////////////////////////////
// currentDate
///////////////////////////////////////////////////////////////////////////////////////////////////

// As $currentDate does not return a constant value, we can't just execute some commands and call
// assert.docEq afterwards. We hence follow a different approach and check the correct insertion of
// the values directly in JavaScript. Furthermore, this test must only be executed as standalone
// cluster, because otherwise $currentDate is written to the oplog yielding different times across
// the cluster.

if (!FixtureHelpers.isReplSet(db) && !FixtureHelpers.isSharded(coll)) {
    const beforeUpdate = new Date();

    coll = resetDb();
    assert.commandWorked(
        coll.updateOne({_id: 1}, {$currentDate: {"scores.1": true, "scores.10": true}}));
    let doc = coll.findOne();

    const afterUpdate = new Date();

    assert(doc.scores[1] instanceof Date);
    assert(doc.scores[10] instanceof Date);

    // Assert updated indexes are Date objects and within delta.
    const deltaMs = 1000;  // 1 second tolerance
    assert(doc.scores[1].getTime() >= beforeUpdate.getTime() &&
           doc.scores[1].getTime() <= afterUpdate.getTime() + deltaMs);

    assert(doc.scores[10].getTime() >= beforeUpdate.getTime() &&
           doc.scores[10].getTime() <= afterUpdate.getTime() + deltaMs);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// unset
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(
    coll.updateOne({_id: 1}, {$unset: {"scores.1": "", "scores.10": "", "scores.12": ""}}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, null, 2, 3, 4, 5, 6, 7, 8, 9, null, 11, null, 13, 14, 15]});

////////////////////////////////////////////////////////////////////////////////////////////////////
// bit
///////////////////////////////////////////////////////////////////////////////////////////////////

coll = resetDb();
assert.commandWorked(coll.updateOne({_id: 1}, {
    $bit: {
        "scores.1": {and: NumberInt(10)},
        "scores.10": {xor: NumberInt(170)},
        "scores.13": {or: NumberInt(7)}
    }
}));
assert.docEq(coll.findOne(),
             {_id: 1, scores: [0, 0, 2, 3, 4, 5, 6, 7, 8, 9, 160, 11, 12, 15, 14, 15]});
