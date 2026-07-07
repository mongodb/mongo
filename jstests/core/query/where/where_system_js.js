// Tests $where use of functions defined in the system.js collection.
// @tags: [
//   # This test expects a function stored in the system.js collection to be available to $where,
//   # which may not be the case if it is implicitly sharded in a passthrough.
//   assumes_unsharded_collection,
//   requires_multi_updates,
//   requires_non_retryable_writes,
//   requires_scripting,
//   requires_system_dot_js_stored_functions,
//   # system.js stored functions only work for collections that live on the db-primary shard so
//   # we have to make sure it wont be moved anywhere by the balancer
//   assumes_balancer_off,
//   backport_required_multiversion,
// ]
const testDB = db.getSiblingDB("where_system_js");
const testColl = testDB.where_system_js;
const systemJsColl = testDB.getCollection("system.js");

assert.commandWorked(testDB.dropDatabase());

assert.commandWorked(
    testColl.insert([
        {x: 1, y: 1},
        {x: 2, y: 1},
    ]),
);

assert.commandWorked(
    testColl.update(
        {
            $where: function () {
                return this.x == 1;
            },
        },
        {$inc: {y: 1}},
        false,
        true,
    ),
);

assert.eq(2, testColl.findOne({x: 1}).y);
assert.eq(1, testColl.findOne({x: 2}).y);

// Test that where queries work with stored javascript
assert.commandWorked(
    systemJsColl.insert({
        _id: "addOne",
        value: function (x) {
            return x + 1;
        },
    }),
);

assert.commandWorked(testColl.update({$where: "addOne(this.x) == 2"}, {$inc: {y: 1}}, false, true));

assert.eq(3, testColl.findOne({x: 1}).y);
assert.eq(1, testColl.findOne({x: 2}).y);

// Test that $where rejects a system.js script of type CodeWithScope.
assert.commandWorked(systemJsColl.insert({_id: "code_with_scope", value: Code("function(){return 1;}", {})}));

assert.commandFailedWithCode(
    testDB.runCommand({find: testColl.getName(), filter: {$where: "code_with_scope(this.x)"}}),
    4546000,
);
// Clean up for the next test.
assert.commandWorked(systemJsColl.deleteOne({_id: "code_with_scope"}));

// Test that a system.js _id cannot inject JS code during scope cleanup.
{
    const numFunctionsBefore = systemJsColl.find({}).itcount();
    // TODO SERVER-130236 improve validation for _id fields to reject this input.
    const craftedId = "x; throw new Error('INJECTED_CODE_EXECUTION') //";
    assert.commandWorked(systemJsColl.insertOne({_id: craftedId, value: 1}));

    // First query loads the 'craftedId' into memory on the pooled scope.
    assert.commandWorked(testDB.runCommand({find: testColl.getName(), filter: {$where: "this.x == 1"}}));

    // Deleting the entry forces the next $where to check if all entries in 'system.js' are still in
    // memory and delete the ones that have been removed.
    assert.commandWorked(systemJsColl.deleteOne({_id: craftedId}));

    // Second query should succeed and we should've removed the deleted entry from scope.
    const res = assert.commandWorked(testDB.runCommand({find: testColl.getName(), filter: {$where: "this.x == 1"}}));
    assert.eq(1, res.cursor.firstBatch.length, "expected one document after cleanup", {res});

    // Confirm the stale entry has been deleted from scope.
    const numFunctions = systemJsColl.find({}).toArray();
    assert.eq(numFunctionsBefore, numFunctions.length, "expected no new entries in system.js namespace", {
        numFunctions,
    });
}
