// Basic examples for $currentDate
var coll = db.update_currentdate;
coll.drop();

// $currentDate default
coll.remove()
coll.save({_id:1, a:2});
coll.update({}, {$currentDate: {a: true}})
assert.gleSuccess(coll.getDB())
assert(coll.findOne().a.constructor == Date)

// $currentDate type = date
coll.remove()
coll.save({_id:1, a:2});
coll.update({}, {$currentDate: {a: {$type: "date"}}})
assert.gleSuccess(coll.getDB())
assert(coll.findOne().a.constructor == Date)

// $currentDate type = timestamp
coll.remove()
coll.save({_id:1, a:2});
coll.update({}, {$currentDate: {a: {$type: "timestamp"}}})
assert.gleSuccess(coll.getDB())
assert(coll.findOne().a.constructor == Timestamp)