// Test basic OR functionality
t = db.stages_or;
t.drop();
var collname = "stages_or";

var N = 50;
for (var i = 0; i < N; ++i) {
    t.insert({foo: i, bar: N - i, baz: i});
}

t.ensureIndex({foo: 1});
t.ensureIndex({bar: 1});
t.ensureIndex({baz: 1});

// baz >= 40
ixscan1 = {
    ixscan: {
        args: {
            keyPattern: {baz: 1},
            startKey: {"": 40},
            endKey: {},
            startKeyInclusive: true,
            endKeyInclusive: true,
            direction: 1
        }
    }
};
// foo >= 40
ixscan2 = {
    ixscan: {
        args: {
            keyPattern: {foo: 1},
            startKey: {"": 40},
            endKey: {},
            startKeyInclusive: true,
            endKeyInclusive: true,
            direction: 1
        }
    }
};

// OR of baz and foo.  Baz == foo and we dedup.
orix1ix2 = {
    or: {args: {nodes: [ixscan1, ixscan2], dedup: true}}
};
res = db.runCommand({stageDebug: {collection: collname, plan: orix1ix2}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 10);

// No deduping, 2x the results.
orix1ix2nodd = {
    or: {args: {nodes: [ixscan1, ixscan2], dedup: false}}
};
res = db.runCommand({stageDebug: {collection: collname, plan: orix1ix2nodd}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 20);
