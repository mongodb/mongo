// Basic examples for $currentDate
var res;
var coll = db.update_currentdate;
coll.drop();

// $currentDate default
coll.remove({});
coll.save({_id: 1, a: 2});
res = coll.update({}, {$currentDate: {a: true}});
assert.writeOK(res);
assert(coll.findOne().a.constructor == Date);

// $currentDate type = date
coll.remove({});
coll.save({_id: 1, a: 2});
res = coll.update({}, {$currentDate: {a: {$type: "date"}}});
assert.writeOK(res);
assert(coll.findOne().a.constructor == Date);

// $currentDate type = timestamp
coll.remove({});
coll.save({_id: 1, a: 2});
res = coll.update({}, {$currentDate: {a: {$type: "timestamp"}}});
assert.writeOK(res);
assert(coll.findOne().a.constructor == Timestamp);
