// @tags: [
//  requires_fastcount,
//  requires_getmore,
//  # Cannot implicitly shard accessed collections because of not being able to create unique
//  # index using hashed shard key pattern.
//  cannot_create_unique_index_when_using_hashed_shard_key,
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
