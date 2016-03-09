// Basic examples for $mul (multiply)
var res;
var coll = db.update_mul;
coll.drop();

// $mul positive
coll.remove({});
coll.save({_id: 1, a: 2});
res = coll.update({}, {$mul: {a: 10}});
assert.writeOK(res);
assert.eq(coll.findOne().a, 20);

// $mul negative
coll.remove({});
coll.save({_id: 1, a: 2});
res = coll.update({}, {$mul: {a: -10}});
assert.writeOK(res);
assert.eq(coll.findOne().a, -20);

// $mul zero
coll.remove({});
coll.save({_id: 1, a: 2});
res = coll.update({}, {$mul: {a: 0}});
assert.writeOK(res);
assert.eq(coll.findOne().a, 0);

// $mul decimal
coll.remove({});
coll.save({_id: 1, a: 2});
res = coll.update({}, {$mul: {a: 1.1}});
assert.writeOK(res);
assert.eq(coll.findOne().a, 2.2);

// $mul negative decimal
coll.remove({});
coll.save({_id: 1, a: 2});
res = coll.update({}, {$mul: {a: -0.1}});
assert.writeOK(res);
assert.eq(coll.findOne().a, -0.2);
