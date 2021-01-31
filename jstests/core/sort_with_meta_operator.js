// Tests to validate the input for sort on '$meta' operator.
(function() {
"use strict";
const coll = db.sort_with_meta_operator;
coll.drop();

assert.commandWorked(
    coll.insert([{_id: 1, p: 1}, {_id: -1, p: -2}, {_id: -2, p: -1}, {_id: 2, p: 2}]));

// Verify that the sort with $meta operator correctly rejects invalid input.
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {_id: {$meta: 1}}}), 31138);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {_id: {$meta: -1}}}),
                             31138);
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), sort: {_id: {$meta: 'searchScore'}}}), 31218);
assert.commandFailedWithCode(
    db.runCommand({find: coll.getName(), sort: {_id: {$meta: 'searchHighlights'}}}), 31219);
assert.commandFailedWithCode(db.runCommand({find: coll.getName(), sort: {_id: {$meta: '1'}}}),
                             31138);

// Verify that sort with $meta:'randVal' works and returns all the documents.
assert.eq(coll.find().sort({_id: {$meta: 'randVal'}}).itcount(), 4);
assert.eq(coll.find().sort({p: {$meta: 'randVal'}, _id: {$meta: 'randVal'}}).itcount(), 4);

// Should still sort on the prefix 'p' when the later part is being sorted with 'randVal'.
assert.eq(coll.find().sort({p: 1, _id: {$meta: 'randVal'}}).toArray(),
          [{_id: -1, p: -2}, {_id: -2, p: -1}, {_id: 1, p: 1}, {_id: 2, p: 2}]);

// Verify the above tests also fail with aggregate.
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), cursor: {}, pipeline: [{$sort: {_id: {$meta: 1}}}]}),
    31138);
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), cursor: {}, pipeline: [{$sort: {_id: {$meta: -1}}}]}),
    31138);
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    cursor: {},
    pipeline: [{$sort: {_id: {$meta: 'searchScore'}}}]
}),
                             31218);
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    cursor: {},
    pipeline: [{$sort: {_id: {$meta: 'searchHighlights'}}}]
}),
                             31219);
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: coll.getName(), cursor: {}, pipeline: [{$sort: {_id: {$meta: '1'}}}]}),
    31138);

// Verify that sort with $meta:'randVal' works and returns all the documents.
assert.eq(coll.aggregate([{$sort: {_id: {$meta: 'randVal'}}}]).itcount(), 4);
assert.eq(coll.aggregate([{$sort: {p: {$meta: 'randVal'}, _id: {$meta: 'randVal'}}}]).itcount(), 4);

// Should still sort on the prefix 'p' when the later part is being sorted with 'randVal'.
assert.eq(coll.aggregate([{$sort: {p: 1, _id: {$meta: 'randVal'}}}]).toArray(),
          [{_id: -1, p: -2}, {_id: -2, p: -1}, {_id: 1, p: 1}, {_id: 2, p: 2}]);
})();
