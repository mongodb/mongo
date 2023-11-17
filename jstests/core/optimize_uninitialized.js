/**
 * Tests that the optimizer deals correctly with uninitialized arguments to an expression.
 * @tags: [requires_fcv_44]
 */
(function() {
const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.createIndexes([{"num": 1}, {"num": -1}]));
assert.eq(coll.find({"num": 10}, {"geo": {$and: ["$n", {$multiply: []}]}}).toArray().length, 0);
})();
