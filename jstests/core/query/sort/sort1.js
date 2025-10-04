// @tags: [
//   requires_getmore,
// ]

let coll = db.sort1;
coll.drop();

coll.save({x: 3, z: 33});
coll.save({x: 5, z: 33});
coll.save({x: 2, z: 33});
coll.save({x: 3, z: 33});
coll.save({x: 1, z: 33});

for (var pass = 0; pass < 2; pass++) {
    assert(coll.find().sort({x: 1})[0].x == 1);
    assert(coll.find().sort({x: 1}).skip(1)[0].x == 2);
    assert(coll.find().sort({x: -1})[0].x == 5);
    assert(coll.find().sort({x: -1})[1].x == 3);
    assert.eq(coll.find().sort({x: -1}).skip(0)[0].x, 5);
    assert.eq(coll.find().sort({x: -1}).skip(1)[0].x, 3);
    coll.createIndex({x: 1});
}

assert(coll.validate().valid);

coll.drop();
coll.save({x: "a"});
coll.save({x: "aba"});
coll.save({x: "zed"});
coll.save({x: "foo"});

for (var pass = 0; pass < 2; pass++) {
    assert.eq("a", coll.find().sort({"x": 1}).limit(1).next().x, "c.1");
    assert.eq("a", coll.find().sort({"x": 1}).next().x, "c.2");
    assert.eq("zed", coll.find().sort({"x": -1}).limit(1).next().x, "c.3");
    assert.eq("zed", coll.find().sort({"x": -1}).next().x, "c.4");
    coll.createIndex({x: 1});
}

assert(coll.validate().valid);

// Ensure that sorts with a collation and no index return the correct ordering. Here we use the
// 'numericOrdering' option which orders number-like strings by their numerical values.
coll.drop();
assert.commandWorked(coll.insert({_id: 0, str: "1000"}));
assert.commandWorked(coll.insert({_id: 1, str: "5"}));
assert.commandWorked(coll.insert({_id: 2, str: "200"}));

let cursor = coll.find().sort({str: -1}).collation({locale: "en_US", numericOrdering: true});
assert.eq(cursor.next(), {_id: 0, str: "1000"});
assert.eq(cursor.next(), {_id: 2, str: "200"});
assert.eq(cursor.next(), {_id: 1, str: "5"});
assert(!cursor.hasNext());

// Ensure that sorting of arrays correctly respects a collation with numeric ordering.
coll.drop();
assert.commandWorked(coll.insert({_id: 0, strs: ["1000", "500"]}));
assert.commandWorked(coll.insert({_id: 1, strs: ["2000", "60"]}));
cursor = coll
    .find({strs: {$lt: "1000"}})
    .sort({strs: 1})
    .collation({
        locale: "en_US",
        numericOrdering: true,
    });
assert.eq(cursor.next(), {_id: 1, strs: ["2000", "60"]});
assert.eq(cursor.next(), {_id: 0, strs: ["1000", "500"]});
assert(!cursor.hasNext());
