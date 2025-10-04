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
