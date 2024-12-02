/**
 * Tests that $_internalFindAllValuesAtPath asserts when provided a non-string input.
 */

const coll = db.find_all_values_at_path_expression;
coll.drop();

// Insert some documents because running aggregation on a non-existent collection on mongos will
// return empty instead of erroring.
let documents = [{a: 4}, {a: 5, b: 1}, {a: 0, b: 1}, {a: 0, b: 1}, {a: 2, b: {c: 1}}];
assert.commandWorked(coll.insert(documents));

assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$replaceRoot: {newRoot: {$_internalFindAllValuesAtPath: null}}},
        {$unwind: {path: "$_internalUnwoundField", preserveNullAndEmptyArrays: true}},
        {$group: {_id: null, distinct: {$addToSet: "$<key>"}}}
    ],
    cursor: {}
}),
                             9567004);

assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$replaceRoot: {newRoot: {$_internalFindAllValuesAtPath: 12345}}},
        {$unwind: {path: "$_internalUnwoundField", preserveNullAndEmptyArrays: true}},
        {$group: {_id: null, distinct: {$addToSet: "$<key>"}}}
    ],
    cursor: {}
}),
                             9567004);
