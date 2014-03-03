// Basic examples for $min/$max
var coll = db.update_min_max;
coll.drop();

// $min for number
coll.insert({_id:1, a:2});
coll.update({_id:1}, {$min: {a: 1}})
assert.gleSuccess(coll.getDB())
assert.eq(coll.findOne({_id:1}).a, 1)

// $max for number
coll.insert({_id:2, a:2});
coll.update({_id:2}, {$max: {a: 1}})
assert.gleSuccess(coll.getDB())
assert.eq(coll.findOne({_id:2}).a, 2)

// $min for Date
coll.insert({_id:3, a: new Date()});
var origDoc = coll.findOne({_id:3})
sleep(2)
coll.update({_id:3}, {$min: {a: new Date()}})
assert.gleSuccess(coll.getDB())
assert.eq(coll.findOne({_id:3}).a, origDoc.a)

// $max for Date
coll.insert({_id:4, a: new Date()});
sleep(2)
var newDate = new Date();
coll.update({_id:4}, {$max: {a: newDate}})
assert.gleSuccess(coll.getDB())
assert.eq(coll.findOne({_id:4}).a, newDate)
