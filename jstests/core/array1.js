t = db.array1;
t.drop();

x = {
    a: [1, 2]
};

t.save({a: [[1, 2]]});
assert.eq(1, t.find(x).count(), "A");

t.save(x);
delete x._id;
assert.eq(2, t.find(x).count(), "B");

t.ensureIndex({a: 1});
assert.eq(2, t.find(x).count(), "C");  // TODO SERVER-146
