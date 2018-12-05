// @tags: [assumes_balancer_off]
load("jstests/libs/fixture_helpers.js");

t = db.index_diag;
t.drop();

assert.commandWorked(t.createIndex({x: 1}));

all = [];
ids = [];
xs = [];

function r(a) {
    var n = [];
    for (var x = a.length - 1; x >= 0; x--)
        n.push(a[x]);
    return n;
}

for (i = 1; i < 4; i++) {
    o = {_id: i, x: -i};
    t.insert(o);
    all.push(o);
    ids.push({_id: i});
    xs.push({x: -i});
}

assert.eq(all, t.find().sort({_id: 1}).toArray());
assert.eq(r(all), t.find().sort({_id: -1}).toArray());

assert.eq(all, t.find().sort({x: -1}).toArray());
assert.eq(r(all), t.find().sort({x: 1}).toArray());

assert.eq(ids, t.find().sort({_id: 1}).returnKey().toArray());
assert.eq(r(ids), t.find().sort({_id: -1}).returnKey().toArray());
assert.eq(xs, t.find().sort({x: -1}).returnKey().toArray());
assert.eq(r(xs), t.find().sort({x: 1}).returnKey().toArray());

// SERVER-4981
if (FixtureHelpers.numberOfShardsForCollection(t) === 1) {
    // With only one shard we can reliably assert on the order of results with a hint, since they
    // will come straight off the index. However, without a sort specified, we cannot make this
    // assertion when there are multiple shards, since mongos can merge results from each shard in
    // whatever order it likes.
    assert.eq(r(xs), t.find().hint({x: 1}).returnKey().toArray());
    assert.commandWorked(t.createIndex({_id: 1, x: 1}));
    assert.eq(all, t.find().hint({_id: 1, x: 1}).returnKey().toArray());
}
assert.commandWorked(t.ensureIndex({_id: 1, x: 1}));
assert.eq(r(all), t.find().hint({_id: 1, x: 1}).sort({x: 1}).returnKey().toArray());

assert.eq([{}, {}, {}], t.find().hint({$natural: 1}).returnKey().toArray());
