// Test limit and skip
t = db.stages_limit_skip;
t.drop();
var collname = "stages_limit_skip";

var N = 50;
for (var i = 0; i < N; ++i) {
    t.insert({foo: i, bar: N - i, baz: i});
}

t.ensureIndex({foo: 1});

// foo <= 20, decreasing
// Limit of 5 results.
ixscan1 = {
    ixscan: {
        args: {
            keyPattern: {foo: 1},
            startKey: {"": 20},
            endKey: {},
            startKeyInclusive: true,
            endKeyInclusive: true,
            direction: -1
        }
    }
};
limit1 = {
    limit: {args: {node: ixscan1, num: 5}}
};
res = db.runCommand({stageDebug: {collection: collname, plan: limit1}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 5);
assert.eq(res.results[0].foo, 20);
assert.eq(res.results[4].foo, 16);

// foo <= 20, decreasing
// Skip 5 results.
skip1 = {
    skip: {args: {node: ixscan1, num: 5}}
};
res = db.runCommand({stageDebug: {collection: collname, plan: skip1}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 16);
assert.eq(res.results[0].foo, 15);
assert.eq(res.results[res.results.length - 1].foo, 0);
