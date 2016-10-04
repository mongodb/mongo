var res;

t = db.push;
t.drop();

t.save({_id: 2, a: [1]});
t.update({_id: 2}, {$push: {a: 2}});
assert.eq("1,2", t.findOne().a.toString(), "A");
t.update({_id: 2}, {$push: {a: 3}});
assert.eq("1,2,3", t.findOne().a.toString(), "B");

t.update({_id: 2}, {$pop: {a: 1}});
assert.eq("1,2", t.findOne().a.toString(), "C");
t.update({_id: 2}, {$pop: {a: -1}});
assert.eq("2", t.findOne().a.toString(), "D");

t.update({_id: 2}, {$push: {a: 3}});
t.update({_id: 2}, {$push: {a: 4}});
t.update({_id: 2}, {$push: {a: 5}});
assert.eq("2,3,4,5", t.findOne().a.toString(), "E1");

t.update({_id: 2}, {$pop: {a: -1}});
assert.eq("3,4,5", t.findOne().a.toString(), "E2");

t.update({_id: 2}, {$pop: {a: -1}});
assert.eq("4,5", t.findOne().a.toString(), "E3");

res = t.update({_id: 2}, {$pop: {a: -1}});
assert.writeOK(res);
assert.eq("5", t.findOne().a.toString(), "E4");

res = t.update({_id: 2}, {$pop: {a: -1}});
assert.writeOK(res);
assert.eq("", t.findOne().a.toString(), "E5");

res = t.update({_id: 2}, {$pop: {a: -1}});
assert.writeOK(res);
assert.eq("", t.findOne().a.toString(), "E6");

res = t.update({_id: 2}, {$pop: {a: -1}});
assert.writeOK(res);
assert.eq("", t.findOne().a.toString(), "E7");

res = t.update({_id: 2}, {$pop: {a: 1}});
assert.writeOK(res);
assert.eq("", t.findOne().a.toString(), "E8");

res = t.update({_id: 2}, {$pop: {b: -1}});
assert.writeOK(res);

res = t.update({_id: 2}, {$pop: {b: 1}});
assert.writeOK(res);
