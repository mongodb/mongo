// This test expects a function stored in the system.js collection to be available for a map/reduce,
// which may not be the case if it is implicitly sharded in a passthrough.
// @tags: [
//     requires_non_retryable_writes,
//     assumes_unsharded_collection,
//
//     # Uses $where operator
//     requires_scripting,
// ]

var myDB = db.getSiblingDB("where4");

myDB.dropDatabase();

assert.commandWorked(myDB.where4.insert({x: 1, y: 1}));
assert.commandWorked(myDB.where4.insert({x: 2, y: 1}));

assert.commandWorked(myDB.where4.update({
    $where: function() {
        return this.x == 1;
    }
},
                                        {$inc: {y: 1}},
                                        false,
                                        true));

assert.eq(2, myDB.where4.findOne({x: 1}).y);
assert.eq(1, myDB.where4.findOne({x: 2}).y);

// Test that where queries work with stored javascript
assert.commandWorked(myDB.system.js.save({
    _id: "where4_addOne",
    value: function(x) {
        return x + 1;
    }
}));

assert.commandWorked(
    myDB.where4.update({$where: "where4_addOne(this.x) == 2"}, {$inc: {y: 1}}, false, true));

assert.eq(3, myDB.where4.findOne({x: 1}).y);
assert.eq(1, myDB.where4.findOne({x: 2}).y);
