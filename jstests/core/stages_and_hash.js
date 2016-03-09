t = db.stages_and_hashed;
t.drop();

var collname = "stages_and_hashed";

var N = 50;
for (var i = 0; i < N; ++i) {
    t.insert({foo: i, bar: N - i, baz: i});
}

t.ensureIndex({foo: 1});
t.ensureIndex({bar: 1});
t.ensureIndex({baz: 1});

// Scan foo <= 20
ixscan1 = {
    ixscan: {
        args: {
            name: "stages_and_hashed",
            keyPattern: {foo: 1},
            startKey: {"": 20},
            endKey: {},
            endKeyInclusive: true,
            direction: -1
        }
    }
};

// Scan bar >= 40
ixscan2 = {
    ixscan: {
        args: {
            name: "stages_and_hashed",
            keyPattern: {bar: 1},
            startKey: {"": 40},
            endKey: {},
            endKeyInclusive: true,
            direction: 1
        }
    }
};

// bar = 50 - foo
// Intersection is (foo=0 bar=50, foo=1 bar=49, ..., foo=10 bar=40)
andix1ix2 = {
    andHash: {args: {nodes: [ixscan1, ixscan2]}}
};
res = db.runCommand({stageDebug: {plan: andix1ix2, collection: collname}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 11);

// Filter predicates from 2 indices.  Tests that we union the idx info.
andix1ix2filter = {
    fetch: {
        filter: {bar: {$in: [45, 46, 48]}, foo: {$in: [4, 5, 6]}},
        args: {node: {andHash: {args: {nodes: [ixscan1, ixscan2]}}}}
    }
};
res = db.runCommand({stageDebug: {collection: collname, plan: andix1ix2filter}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 2);
