
t = db.jstests_js5;
t.drop();

t.save({a: 1});
t.save({a: 2});

assert.eq(2, t.find({"$where": "this.a"}).count(), "A");
assert.eq(0, t.find({"$where": "this.b"}).count(), "B");
assert.eq(0, t.find({"$where": "this.b > 45"}).count(), "C");
