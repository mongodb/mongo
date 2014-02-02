// Basic examples for $min/$max
var coll = db.update_min_max;
coll.drop();

// $min for number
coll.remove({})
coll.save({_id:1, a:2});
coll.update({}, {$min: {a: 1}})
assert.gleSuccess(coll.getDB())
assert.eq(coll.findOne().a, 1)

// $max for number
coll.remove({})
coll.save({_id:1, a:2});
coll.update({}, {$max: {a: 1}})
assert.gleSuccess(coll.getDB())
assert.eq(coll.findOne().a, 2)

// $min for Date
coll.remove({})
coll.save({_id:1, a: new Date()});
var origDoc = coll.findOne()
sleep(2)
coll.update({}, {$min: {a: new Date()}})
assert.gleSuccess(coll.getDB())
assert.eq(coll.findOne().a, origDoc.a)

// $max for Date
coll.remove({})
coll.save({_id:1, a: new Date()});
sleep(2)
var newDate = new Date();
coll.update({}, {$max: {a: newDate}})
assert.gleSuccess(coll.getDB())
assert.eq(coll.findOne().a, newDate)
