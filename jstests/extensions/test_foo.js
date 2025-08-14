/**
 * Tests that $testFoo (a noop extensions stage) works E2E after mongod is started with
 * libfoo_mongo_extension.so successfully loaded.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

// Basic sanity check to ensure the server is operational.
const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insertMany([{x: 1}, {x: 2}, {x: 3}]));

// $testFoo noop stage should successfully pass documents to next stage.
const res =
    coll.aggregate(
            [{$match: {x: {$gte: 2}}}, {$testFoo: {}}, {$group: {_id: null, cnt: {$sum: 1}}}])
        .toArray();

assert.eq(res.length, 1, tojson(res));
assert.eq(res, [{_id: null, cnt: 2}]);
