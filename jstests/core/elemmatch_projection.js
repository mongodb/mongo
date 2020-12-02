// Tests for $elemMatch projection operator.
// @tags: [
//   requires_getmore,
// ]
(function() {
"use strict";

const coll = db.elemmatch_projection;
coll.drop();

function testSingleDocument(projection, input, expectedOutput, deleteId = true) {
    assert.commandWorked(coll.insert(input));
    const actualOutput = coll.findOne({}, projection);
    if (deleteId) {
        delete actualOutput._id;
    }
    assert.eq(actualOutput, expectedOutput);
    assert(coll.drop());
}

// Single object match.
testSingleDocument(
    {x: {$elemMatch: {a: -6}}}, {x: [{a: 1, b: 4}, {a: -6, c: 3}]}, {x: [{a: -6, c: 3}]});
testSingleDocument(
    {x: {$elemMatch: {a: -6, c: 3}}}, {x: [{a: 1, b: 4}, {a: -6, c: 3}]}, {x: [{a: -6, c: 3}]});
testSingleDocument(
    {x: {$elemMatch: {a: {$lt: 1}}}}, {x: [{a: 1, b: 4}, {a: -6, c: 3}]}, {x: [{a: -6, c: 3}]});

// $elemMatch mast return only the first matching element.
testSingleDocument({x: {$elemMatch: {a: {$gt: 0, $lt: 2}}}},
                   {x: [{a: 1, b: 1}, {a: 1, b: 2}]},
                   {x: [{a: 1, b: 1}]});

// $in operator in $elemMatch predicate.
testSingleDocument({x: {$elemMatch: {$in: [100, 4, -123]}}}, {x: [1, 2, 3, 4, 5]}, {x: [4]});
testSingleDocument({x: {$elemMatch: {a: {$in: [1]}}}},
                   {x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}]},
                   {x: [{a: 1, b: 2}]});
testSingleDocument({x: {$elemMatch: {$nin: [4, 5, 6]}}}, {x: [1, 2, 3, 4, 5]}, {x: [1]});
testSingleDocument({x: {$elemMatch: {$all: [1]}}}, {x: [1, 2, 3, 4, 5]}, {x: [1]});

// Empty $elemMatch predicate must match first array or object.
testSingleDocument(
    {x: {$elemMatch: {}}}, {x: ['string', 123, [1, 2, 3], {a: 1}]}, {x: [[1, 2, 3]]});
testSingleDocument({x: {$elemMatch: {}}}, {x: ['string', 123, {a: 1}]}, {x: [{a: 1}]});

// Nested $elemMatch.
testSingleDocument({x: {$elemMatch: {$elemMatch: {$gt: 5, $lt: 7}}}},
                   {x: [[1, 2, 3], [4, 5, 6], [7, 8, 9]]},
                   {x: [[4, 5, 6]]});

// Various types of predicates.
const fixedDate = new Date();

testSingleDocument({x: {$elemMatch: {a: 'string'}}},
                   {
                       x: [
                           {a: 'string', b: fixedDate},
                           {a: new ObjectId(), b: 1.2345},
                           {a: 'string2', b: fixedDate}
                       ]
                   },
                   {x: [{a: 'string', b: fixedDate}]});
testSingleDocument({x: {$elemMatch: {a: /ring2/}}},
                   {
                       x: [
                           {a: 'string', b: fixedDate},
                           {a: new ObjectId(), b: 1.2345},
                           {a: 'string2', b: fixedDate}
                       ]
                   },
                   {x: [{a: 'string2', b: fixedDate}]});
testSingleDocument({x: {$elemMatch: {a: {$type: 2}}}},
                   {
                       x: [
                           {a: 'string', b: fixedDate},
                           {a: new ObjectId(), b: 1.2345},
                           {a: 'string2', b: fixedDate}
                       ]
                   },
                   {x: [{a: 'string', b: fixedDate}]});
testSingleDocument({x: {$elemMatch: {a: {$ne: 1}}}},
                   {x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}]},
                   {x: [{a: 2, c: 3}]});
testSingleDocument({x: {$elemMatch: {d: {$exists: true}}}},
                   {
                       x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}],
                       y: [{aa: 1, bb: 2}, {aa: 2, cc: 3}, {aa: 1, dd: 5}]
                   },
                   {x: [{a: 1, d: 5}]});
testSingleDocument({x: {$elemMatch: {a: {$mod: [2, 0]}}}},
                   {
                       x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}],
                       y: [{aa: 1, bb: 2}, {aa: 2, cc: 3}, {aa: 1, dd: 5}]
                   },
                   {x: [{a: 2, c: 3}]});
testSingleDocument({_id: 0, x: {$elemMatch: {a: 1}}, y: {$elemMatch: {c: 3}}},
                   {x: [{a: 1, b: 2}, {a: 3, b: 4}], y: [{c: 1, d: 2}, {c: 3, d: 4}]},
                   {x: [{a: 1, b: 2}], y: [{c: 3, d: 4}]});
testSingleDocument({x: {$elemMatch: {$gt: 2, $lt: 4}}}, {x: [1, 2, 4, 3, 5]}, {x: [3]});
testSingleDocument(
    {x: {$elemMatch: {$or: [{a: {$eq: 4}}, {$and: [{a: {$mod: [12, 0]}}, {a: {$mod: [15, 0]}}]}]}}},
    {x: [{a: 1}, {a: 12 * 15}, {a: 4}]},
    {x: [{a: 12 * 15}]});

// $elemMatch on a DBRef field.
testSingleDocument({x: {$elemMatch: {$id: "id0"}}},
                   {
                       x: [
                           new DBRef("otherCollection", "id0", db.getName()),
                           new DBRef("otherCollection", "id1", db.getName()),
                           new DBRef("otherCollection2", "id2", db.getName())
                       ]
                   },
                   {x: [new DBRef("otherCollection", "id0", db.getName())]});

testSingleDocument({x: {$elemMatch: {$ref: "otherCollection2"}}},
                   {
                       x: [
                           new DBRef("otherCollection", "id0", db.getName()),
                           new DBRef("otherCollection", "id1", db.getName()),
                           new DBRef("otherCollection2", "id2", db.getName())
                       ]
                   },
                   {x: [new DBRef("otherCollection2", "id2", db.getName())]});

testSingleDocument({x: {$elemMatch: {$db: db.getName()}}},
                   {
                       x: [
                           new DBRef("otherCollection", "id0", db.getName()),
                           new DBRef("otherCollection", "id1", db.getName()),
                           new DBRef("otherCollection2", "id2", db.getName())
                       ]
                   },
                   {x: [new DBRef("otherCollection", "id0", db.getName())]});

// Test that if $elemMatch is applied to non-array value, field is omitted from the output.
testSingleDocument({x: {$elemMatch: {$gt: 2}}}, {x: {a: 123}, d: 456}, {});
testSingleDocument({x: {$elemMatch: {$gt: 2}}, d: 1}, {x: {a: 123}, d: 456}, {d: 456});
testSingleDocument({x: {$elemMatch: {$gt: 2}}, d: 0}, {x: {a: 123}, d: 456}, {});

// Test $elemMatch with empty predicate.
testSingleDocument({x: {$elemMatch: {}}}, {x: ["string", 123, {b: 123}]}, {x: [{b: 123}]});
testSingleDocument({x: {$elemMatch: {}}}, {x: ["string", 123, [1, 2, 3]]}, {x: [[1, 2, 3]]});
testSingleDocument(
    {x: {$elemMatch: {$or: [{}, {}]}}}, {x: ["string", 123, {b: 123}]}, {x: [{b: 123}]});
testSingleDocument(
    {x: {$elemMatch: {$or: [{}, {}]}}}, {x: ["string", 123, [1, 2, 3]]}, {x: [[1, 2, 3]]});

// Test $elemMatch with no matching elements.
testSingleDocument({x: {$elemMatch: {$lt: 0}}}, {x: [1, 2, 3]}, {});

// Test the $elemMatch operator across multiple batches.
assert.commandWorked(coll.insert([
    {
        _id: 0,
        x: [{a: 1, b: 2}, {a: 2, c: 3}, {a: 1, d: 5}],
        y: [{aa: 1, bb: 2}, {aa: 2, cc: 3}, {aa: 1, dd: 5}]
    },
    {_id: 1, x: [{a: 1, b: 3}, {a: -6, c: 3}]},
]));

const it = coll.find({}, {x: {$elemMatch: {a: 1}}}).sort({_id: 1}).batchSize(1);
while (it.hasNext()) {
    const currentDocument = it.next();
    assert.eq(currentDocument.x[0].a, 1);
}

assert(coll.drop());
}());
