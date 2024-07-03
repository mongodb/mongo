// Regression test to check that we clean up missing Values from arrays.

const queryProjectAddFields = [
    // $group is needed to force our Document to not be backed by BSONObj
    {$group: {_id: null, array: {$first: "$array"}}},
    // Inclusion projection on a sub-field of an array removes all scalar values from the array
    {$project: {_id: 0, "array.m1": 1}},
    // $addFields will iterate over the array and add m2: "what?" field to each elememt. However,
    // because scalars were removed, only elements that were objects should be preserved.
    {$addFields: {"array.m2": "what?"}},
];

const queryProjectOnly = [
    {$group: {_id: null, array: {$first: "$array"}}},
    {$project: {_id: 0, "array.m1": 1}},
];

const queryAddFieldsOnly = [
    {$group: {_id: null, array: {$first: "$array"}}},
    {$addFields: {"array.m2": "what?"}},
];

const queryProjectWithExpression = [
    {$group: {_id: null, array: {$first: "$array"}}},
    {$project: {_id: 0, "array.m1": 1, "array.m2": "what?"}},
];

const coll = db.add_fields_on_missing_array_values;
coll.drop();

const doc = {
    _id: 0,
    array: [1, 2, {m1: "str"}]
};
assert.commandWorked(coll.insertOne(doc));

assert.eq([{array: [{m1: "str", m2: "what?"}]}], coll.aggregate(queryProjectAddFields).toArray());
assert.eq([{array: [{m1: "str"}]}], coll.aggregate(queryProjectOnly).toArray());
assert.eq([{_id: null, array: [{m2: "what?"}, {m2: "what?"}, {m1: "str", m2: "what?"}]}],
          coll.aggregate(queryAddFieldsOnly).toArray());
assert.eq([{array: [{m2: "what?"}, {m2: "what?"}, {m1: "str", m2: "what?"}]}],
          coll.aggregate(queryProjectWithExpression).toArray());
