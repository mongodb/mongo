var res;

t = db.unset2;
t.drop();

t.save({a: ["a", "b", "c", "d"]});
t.update({}, {$unset: {"a.3": 1}});
assert.eq(["a", "b", "c", null], t.findOne().a);
t.update({}, {$unset: {"a.1": 1}});
assert.eq(["a", null, "c", null], t.findOne().a);
t.update({}, {$unset: {"a.0": 1}});
assert.eq([null, null, "c", null], t.findOne().a);
t.update({}, {$unset: {"a.4": 1}});
assert.eq([null, null, "c", null], t.findOne().a);  // no change

t.drop();
t.save({a: ["a", "b", "c", "d", "e"]});
t.update({}, {$unset: {"a.2": 1}, $set: {"a.3": 3, "a.4": 4, "a.5": 5}});
assert.eq(["a", "b", null, 3, 4, 5], t.findOne().a);

t.drop();
t.save({a: ["a", "b", "c", "d", "e"]});
res = t.update({}, {$unset: {"a.2": 1}, $set: {"a.2": 4}});
assert.writeError(res);
assert.eq(["a", "b", "c", "d", "e"], t.findOne().a);
