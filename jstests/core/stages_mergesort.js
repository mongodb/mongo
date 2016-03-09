// Test query stage merge sorting.
t = db.stages_mergesort;
t.drop();
var collname = "stages_mergesort";

var N = 10;
for (var i = 0; i < N; ++i) {
    t.insert({foo: 1, bar: N - i - 1});
    t.insert({baz: 1, bar: i});
}

t.ensureIndex({foo: 1, bar: 1});
t.ensureIndex({baz: 1, bar: 1});

// foo == 1
// We would (internally) use "": MinKey and "": MaxKey for the bar index bounds.
ixscan1 = {
    ixscan: {
        args: {
            keyPattern: {foo: 1, bar: 1},
            startKey: {foo: 1, bar: 0},
            endKey: {foo: 1, bar: 100000},
            endKeyInclusive: true,
            direction: 1
        }
    }
};
// baz == 1
ixscan2 = {
    ixscan: {
        args: {
            keyPattern: {baz: 1, bar: 1},
            startKey: {baz: 1, bar: 0},
            endKey: {baz: 1, bar: 100000},
            endKeyInclusive: true,
            direction: 1
        }
    }
};

mergesort = {
    mergeSort: {args: {nodes: [ixscan1, ixscan2], pattern: {bar: 1}}}
};
res = db.runCommand({stageDebug: {plan: mergesort, collection: collname}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 2 * N);
assert.eq(res.results[0].bar, 0);
assert.eq(res.results[2 * N - 1].bar, N - 1);
