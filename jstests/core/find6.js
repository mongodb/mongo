
t = db.find6;
t.drop();

t.save({a: 1});
t.save({a: 1, b: 1});

assert.eq(2, t.find().count(), "A");
assert.eq(1, t.find({b: null}).count(), "B");
assert.eq(1, t.find("function() { return this.b == null; }").itcount(), "C");
assert.eq(1, t.find("function() { return this.b == null; }").count(), "D");

/* test some stuff with dot array notation */
q = db.find6a;
q.drop();
q.insert({"a": [{"0": 1}]});
q.insert({"a": [{"0": 2}]});
q.insert({"a": [1]});
q.insert({"a": [9, 1]});

function f() {
    assert.eq(2, q.find({'a.0': 1}).count(), "da1");
    assert.eq(2, q.find({'a.0': 1}).count(), "da2");

    assert.eq(1, q.find({'a.0': {$gt: 8}}).count(), "da3");
    assert.eq(0, q.find({'a.0': {$lt: 0}}).count(), "da4");
}

for (var pass = 0; pass <= 1; pass++) {
    f();
    q.ensureIndex({a: 1});
}

t = db.multidim;
t.drop();
t.insert({"a": [[], 1, [3, 4]]});
assert.eq(1, t.find({"a.2": [3, 4]}).count(), "md1");
assert.eq(1, t.find({"a.2.1": 4}).count(), "md2");
assert.eq(0, t.find({"a.2.1": 3}).count(), "md3");
