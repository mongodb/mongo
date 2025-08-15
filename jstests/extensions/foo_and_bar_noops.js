/**
 * Tests that $testFoo and $testBar (no-op extension stages) work E2E after mongod is started with
 * libfoo_mongo_extension.so and libbar_mongo_extension.so successfully loaded.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {assertArrayEq, assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();
const testData = [{_id: 0, x: 1}, {_id: 1, x: 2}, {_id: 2, x: 3}];
assert.commandWorked(coll.insertMany(testData));

// Test one no-op stage passes documents through unchanged.
{
    const pipeline = [{$testFoo: {}}];
    const result = coll.aggregate(pipeline).toArray();

    assertArrayEq({actual: result, expected: testData});
}

// Test two different no-op stages passes documents through unchanged.
{
    const pipeline = [{$testFoo: {}}, {$testBar: {number: 42}}];
    const result = coll.aggregate(pipeline).toArray();

    assertArrayEq({actual: result, expected: testData});
}

// Test $testFoo stage fails to parse.
{
    const pipeline = [{$testFoo: {invalidField: "value"}}];
    assertErrorCode(
        coll, pipeline, 10624200, "Using $testFoo with invalid field should be rejected");
}

// Test $testBar stage fails to parse.
{
    const pipeline = [{$testBar: {}}];
    assertErrorCode(
        coll, pipeline, 10785800, "Using $testBar with empty object should be rejected");
}

// Test no-op stages throughout a pipeline.
{
    const pipeline =
        [{$testBar: {anyField: true}}, {$match: {x: {$gte: 2}}}, {$testFoo: {}}, {$sort: {x: 1}}];
    const result = coll.aggregate(pipeline).toArray();

    assertArrayEq({actual: result, expected: [{_id: 1, x: 2}, {_id: 2, x: 3}]});
}

// Test no-op stage with a subsequent stage that modifies documents.
{
    const pipeline =
        [{$match: {x: {$gte: 2}}}, {$testFoo: {}}, {$group: {_id: null, cnt: {$sum: 1}}}];
    const result = coll.aggregate(pipeline).toArray();

    assertArrayEq({actual: result, expected: [{_id: null, cnt: 2}]});
}

// Test multiple no-op stages in sequence.
{
    const pipeline = [
        {$testBar: {bits: "bobs"}},
        {$testFoo: {}},
        {$testFoo: {}},
        {$testBar: {number: 5}},
        {$testFoo: {}},
        {$testBar: {number: 5, think: "big", go: "far"}},
    ];
    const result = coll.aggregate(pipeline).toArray();

    assertArrayEq({actual: result, expected: testData});
}

// Test no-op stage with empty collection.
{
    const emptyColl = db.extensions_empty_test;
    emptyColl.drop();

    const pipeline = [{$testFoo: {}}, {$testBar: {who: "halal guys"}}];
    const result = emptyColl.aggregate(pipeline).toArray();

    assert.eq(result.length, 0, result);
}

// Test no-op stage at different positions in complex pipeline.
{
    const pipeline = [
        {$testFoo: {}},
        {$match: {x: {$in: [1, 3]}}},
        {$testFoo: {}},
        {$project: {y: "$x", _id: 0}},
        {$testBar: {a: 0}},
        {$sort: {y: -1}}
    ];
    const result = coll.aggregate(pipeline).toArray();

    assertArrayEq({actual: result, expected: [{y: 3}, {y: 1}]});
}

// Test no-op stage with complex nested documents.
{
    const nestedColl = db.extensions_nested_test;
    nestedColl.drop();

    const nestedDoc = {
        _id: 1,
        docInfo: {
            title: "some_doc",
            version: 2,
            authors: [{name: "John Doe", role: "author"}, {name: "Jane Doe", role: "editor"}]
        }
    };

    assert.commandWorked(nestedColl.insertOne(nestedDoc));

    const pipeline = [{$testFoo: {}}, {$testBar: {author: "Doe"}}];
    const result = nestedColl.aggregate(pipeline).toArray();

    assertArrayEq({actual: result, expected: [nestedDoc]});
}
