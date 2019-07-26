/*
 * SERVER-6147 : aggregation $ne expression applied to constant returns incorrect result.
 *
 * This test validates the SERVER-6147 ticket. Return true when comparing a constant to a field
 * containing a different value using $ne. Previously it would return false when comparing a
 * constant and a field regardless of whether they were equal or not.
 */
(function() {
"use strict";
db.s6147.drop();

assert.writeOK(db.s6147.insert({a: 1}));
assert.writeOK(db.s6147.insert({a: 2}));

// Aggregate checking various combinations of the constant and the field.
const cursor = db.s6147.aggregate([
    {$sort: {a: 1}},
    {
        $project: {
            _id: 0,
            constantAndField: {$ne: [1, "$a"]},
            fieldAndConstant: {$ne: ["$a", 1]},
            constantAndConstant: {$ne: [1, 1]},
            fieldAndField: {$ne: ["$a", "$a"]}
        }
    }
]);

// In both documents, the constantAndConstant and fieldAndField should be false since they
// compare something with itself. However, the constantAndField and fieldAndConstant should be
// different as document one contains 1 which should return false and document 2 contains
// something different so should return true.
const expected = [
    {
        constantAndField: false,
        fieldAndConstant: false,
        constantAndConstant: false,
        fieldAndField: false
    },
    {
        constantAndField: true,
        fieldAndConstant: true,
        constantAndConstant: false,
        fieldAndField: false
    }
];

assert.eq(cursor.toArray(), expected);
}());
