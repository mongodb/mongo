// A few rooted $or cases.

var t = db.jstests_orq;
t.drop();

t.ensureIndex({a: 1, c: 1});
t.ensureIndex({b: 1, c: 1});

t.save({a: 1, c: 9});
t.save({a: 1, c: 10});
t.save({b: 2, c: 8});
t.save({b: 2, c: 7});

// This can be answered using a merge sort. See SERVER-13715.
var cursor = t.find({$or: [{a: 1}, {b: 2}]}).sort({c: 1});
for (var i = 7; i < 11; i++) {
    assert.eq(i, cursor.next()["c"]);
}
assert(!cursor.hasNext());

// SERVER-13715
assert.eq(4, t.find({$or: [{a: 1}, {b: 2}]}).sort({a: 1}).itcount());
