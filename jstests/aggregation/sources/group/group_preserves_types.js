/**
 * Tests that $group preserves the BSON type of JavaScript (Code) and Symbol values when its output
 * is consumed by a later stage, whether the value is a top-level field of the group output, nested
 * in an object, or nested in an array.
 *
 * @tags: [
 *   requires_fcv_91,
 * ]
 */
const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insert({_id: 0, code: new Code("x = 1;")}));

// The shell cannot construct Symbol values, so build one on the server.
assert.commandWorked(
    coll.updateOne({_id: 0}, [
        {$set: {sym: {$deserializeEJSON: {input: {$literal: {$symbol: "s"}}}}}},
    ]),
);

const cases = [
    {field: "$code", type: "javascript"},
    {field: "$sym", type: "symbol"},
];
for (const {field, type} of cases) {
    const group = {$group: {_id: {key: field}, first: {$first: field}, all: {$push: field}}};
    const project = {
        $project: {
            _id: 0,
            idType: {$type: "$_id.key"},
            firstType: {$type: "$first"},
            allTypes: {$map: {input: "$all", in: {$type: "$$this"}}},
        },
    };

    const expectedResults = [{idType: type, firstType: type, allTypes: [type]}];
    assert.eq(coll.aggregate([group, project]).toArray(), expectedResults, {field});
    assert.eq(
        coll.aggregate([group, {$_internalInhibitOptimization: {}}, project]).toArray(),
        expectedResults,
        {field},
    );
}
