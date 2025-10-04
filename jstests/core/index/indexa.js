// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//  assumes_no_implicit_index_creation,
//  requires_fastcount,
//  requires_getmore,
//]

// unique index constraint test for updates
// case where object doesn't grow tested here

let t = db.indexa;
t.drop();

t.createIndex({x: 1}, true);

t.insert({"x": "A"});
t.insert({"x": "B"});
t.insert({"x": "A"});

assert.eq(2, t.count(), "indexa 1");

t.update({x: "B"}, {x: "A"});

let a = t.find().toArray();
let u = Array.unique(
    a.map(function (z) {
        return z.x;
    }),
);
assert.eq(2, t.count(), "indexa 2");

assert(a.length == u.length, "unique index update is broken");
