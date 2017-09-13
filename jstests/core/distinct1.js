
t = db.distinct1;
t.drop();

assert.eq(0, t.distinct("a").length, "test empty");

t.save({a: 1});
t.save({a: 2});
t.save({a: 2});
t.save({a: 2});
t.save({a: 3});

res = t.distinct("a");
assert.eq("1,2,3", res.toString(), "A1");

assert.eq("1,2", t.distinct("a", {a: {$lt: 3}}), "A2");

t.drop();

t.save({a: {b: "a"}, c: 12});
t.save({a: {b: "b"}, c: 12});
t.save({a: {b: "c"}, c: 12});
t.save({a: {b: "c"}, c: 12});

res = t.distinct("a.b");
assert.eq("a,b,c", res.toString(), "B1");

t.drop();

t.save({_id: 1, a: 1});
t.save({_id: 2, a: 2});

// Test distinct with _id.
res = t.distinct("_id");
assert.eq("1,2", res.toString(), "C1");
res = t.distinct("a", {_id: 1});
assert.eq("1", res.toString(), "C2");

// Test distinct with db.runCommand
t.drop();

t.save({a: 1, b: 2});
t.save({a: 2, b: 2});
t.save({a: 2, b: 1});
t.save({a: 2, b: 2});
t.save({a: 3, b: 2});
t.save({a: 4, b: 1});
t.save({a: 4, b: 1});

res = db.runCommand({distinct: "distinct1", key: "a"});
assert.commandWorked(res);
assert.eq([1, 2, 3, 4], res["values"], "D1");
res = db.runCommand({distinct: "distinct1", key: "a", query: null});
assert.commandWorked(res);
assert.eq([1, 2, 3, 4], res["values"], "D2");
res = db.runCommand({distinct: "distinct1", key: "a", query: {b: 2}});
assert.commandWorked(res);
assert.eq([1, 2, 3], res["values"], "D3");
res = db.runCommand({distinct: "distinct1", key: "a", query: 1});
assert.commandFailed(res);

t.drop();
