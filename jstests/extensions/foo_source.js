/**
 * Tests that $testFooSource (source extension stage) work E2E after mongod is started with
 * libfoo_source_mongo_extension.so successfully loaded.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {assertArrayEq, assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();
const testData = [
    {_id: 0, x: 1},
    {_id: 1, x: 2},
    {_id: 2, x: 3},
];
assert.commandWorked(coll.insertMany(testData));

// Test one no-op stage passes documents through unchanged.
{
    const pipeline = [{$testFooSource: {}}];
    const result = coll.aggregate(pipeline).toArray();

    assertArrayEq({actual: result, expected: testData});
}

// Test $testFoo stage fails to parse.
{
    const pipeline = [{$testFooSource: {invalidField: "value"}}];
    assertErrorCode(coll, pipeline, 11165101, "Using $testFooSource with invalid field should be rejected");
}
