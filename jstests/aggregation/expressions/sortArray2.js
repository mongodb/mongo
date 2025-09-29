// @tags: [
//   requires_fcv_83,
// ]

// SERVER-109190: verify sorting works in the same way $sort works, also in presence of fields
// containing dots.
let coll = db.sortArray2;
coll.drop();

let assertDBOutputEquals = (expected, output) => {
    output = output.toArray();
    assert.eq(1, output.length);
    assert.eq(expected, output[0].sorted);
};

assert.commandWorked(
    coll.insertMany([
        {_id: 0, b: {c: 1}},
        {_id: 1, b: {c: 2}},
    ]),
);
assertDBOutputEquals(
    coll.aggregate([{$sort: {"b.c": 1}}]).toArray(),
    coll.aggregate([
        {$group: {_id: null, arr: {$push: "$$ROOT"}}},
        {$project: {_id: 0, sorted: {$sortArray: {input: "$arr", sortBy: {"b.c": 1}}}}},
    ]),
);
assertDBOutputEquals(
    coll.aggregate([{$sort: {"b.c": -1}}]).toArray(),
    coll.aggregate([
        {$group: {_id: null, arr: {$push: "$$ROOT"}}},
        {$project: {_id: 0, sorted: {$sortArray: {input: "$arr", sortBy: {"b.c": -1}}}}},
    ]),
);

assert.commandWorked(
    coll.insertMany([
        {_id: 2, "b.c": 1, b: {c: 3}},
        {_id: 3, "b.c": 0, b: {c: 4}},
    ]),
);
assertDBOutputEquals(
    coll.aggregate([{$sort: {"b.c": 1}}]).toArray(),
    coll.aggregate([
        {$group: {_id: null, arr: {$push: "$$ROOT"}}},
        {$project: {_id: 0, sorted: {$sortArray: {input: "$arr", sortBy: {"b.c": 1}}}}},
    ]),
);
