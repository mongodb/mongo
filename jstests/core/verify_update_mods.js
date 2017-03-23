// Verify update mods exist
var res;
t = db.update_mods;
t.drop();

t.save({_id: 1});
res = t.update({}, {$set: {a: 1}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$unset: {a: 1}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$inc: {a: 1}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$mul: {a: 1}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$push: {a: 1}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$pushAll: {a: [1]}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$addToSet: {a: 1}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$pull: {a: 1}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$pop: {a: true}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$rename: {a: "b"}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$bit: {a: {and: NumberLong(1)}}});
assert.writeOK(res);
t.remove({});

// SERVER-3223 test $bit can do an upsert
t.update({_id: 1}, {$bit: {a: {and: NumberLong(3)}}}, true);
assert.eq(t.findOne({_id: 1}).a, NumberLong(0), "$bit upsert with and");
t.update({_id: 2}, {$bit: {b: {or: NumberLong(3)}}}, true);
assert.eq(t.findOne({_id: 2}).b, NumberLong(3), "$bit upsert with or (long)");
t.update({_id: 3}, {$bit: {"c.d": {or: NumberInt(3)}}}, true);
assert.eq(t.findOne({_id: 3}).c.d, NumberInt(3), "$bit upsert with or (int)");
t.remove({});

t.save({_id: 1});
res = t.update({}, {$currentDate: {a: true}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$max: {a: 1}});
assert.writeOK(res);
t.remove({});

t.save({_id: 1});
res = t.update({}, {$min: {a: 1}});
assert.writeOK(res);
t.remove({});
