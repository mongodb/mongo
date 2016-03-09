
t = db.index_many2;
t.drop();

t.save({x: 1});

assert.eq(1, t.getIndexKeys().length, "A1");

function make(n) {
    var x = {};
    x["x" + n] = 1;
    return x;
}

for (i = 1; i < 1000; i++) {
    t.ensureIndex(make(i));
}

assert.eq(64, t.getIndexKeys().length, "A2");

num = t.getIndexKeys().length;

t.dropIndex(make(num - 1));
assert.eq(num - 1, t.getIndexKeys().length, "B0");

t.ensureIndex({z: 1});
assert.eq(num, t.getIndexKeys().length, "B1");

t.dropIndex("*");
assert.eq(1, t.getIndexKeys().length, "C1");
