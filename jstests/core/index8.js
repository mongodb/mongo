// Test key uniqueness

t = db.jstests_index8;
t.drop();

t.ensureIndex({a: 1});
t.ensureIndex({b: 1}, true);
t.ensureIndex({c: 1}, [false, "cIndex"]);

checkIndexes = function(num) {
    var indexes = t.getIndexes();
    assert.eq(4, indexes.length);

    var start = 0;
    if (indexes[0].name == "_id_")
        start = 1;
    assert(!indexes[start].unique, "A" + num);
    assert(indexes[start + 1].unique, "B" + num + " " + tojson(indexes[start + 1]));
    assert(!indexes[start + 2].unique, "C" + num);
    assert.eq("cIndex", indexes[start + 2].name, "D" + num);
};

checkIndexes(1);
t.reIndex();
checkIndexes(2);

t.save({a: 2, b: 1});
t.save({a: 2});
assert.eq(2, t.find().count());

t.save({b: 4});
t.save({b: 4});
assert.eq(3, t.find().count());
assert.eq(3, t.find().hint({c: 1}).toArray().length);
assert.eq(3, t.find().hint({b: 1}).toArray().length);
assert.eq(3, t.find().hint({a: 1}).toArray().length);

t.drop();
t.ensureIndex({a: 1, b: -1}, true);
t.save({a: 2, b: 3});
t.save({a: 2, b: 3});
t.save({a: 2, b: 4});
t.save({a: 1, b: 3});
assert.eq(3, t.find().count());

t.drop();
t.ensureIndex({a: 1}, true);
t.save({a: [2, 3]});
t.save({a: 2});
assert.eq(1, t.find().count());

t.drop();
t.ensureIndex({a: 1}, true);
t.save({a: 2});
t.save({a: [1, 2, 3]});
t.save({a: [3, 2, 1]});
assert.eq(1, t.find().sort({a: 1}).hint({a: 1}).toArray().length);
assert.eq(1, t.find().sort({a: -1}).hint({a: 1}).toArray().length);

assert.eq(t._indexSpec({x: 1}, true), t._indexSpec({x: 1}, [true]), "spec 1");
assert.eq(t._indexSpec({x: 1}, "eliot"), t._indexSpec({x: 1}, ["eliot"]), "spec 2");
