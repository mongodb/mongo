t = db.stages_and_sorted;
t.drop();
var collname = "stages_and_sorted";

var N = 10;
for (var i = 0; i < N; ++i) {
    // These will show up in the index scans below but must not be outputted in the and.
    t.insert({foo: 1});
    t.insert({foo: 1, bar: 1});
    t.insert({baz: 12});
    t.insert({bar: 1});
    // This is the only thing that should be outputted in the and.
    t.insert({foo: 1, bar: 1, baz: 12});
    t.insert({bar: 1});
    t.insert({bar: 1, baz: 12});
    t.insert({baz: 12});
    t.insert({foo: 1, baz: 12});
    t.insert({baz: 12});
}

t.ensureIndex({foo: 1});
t.ensureIndex({bar: 1});
t.ensureIndex({baz: 1});

// Scan foo == 1
ixscan1 = {
    ixscan: {
        args: {
            name: "stages_and_sorted",
            keyPattern: {foo: 1},
            startKey: {"": 1},
            endKey: {"": 1},
            endKeyInclusive: true,
            direction: 1
        }
    }
};

// Scan bar == 1
ixscan2 = {
    ixscan: {
        args: {
            name: "stages_and_sorted",
            keyPattern: {bar: 1},
            startKey: {"": 1},
            endKey: {"": 1},
            endKeyInclusive: true,
            direction: 1
        }
    }
};

// Scan baz == 12
ixscan3 = {
    ixscan: {
        args: {
            name: "stages_and_sorted",
            keyPattern: {baz: 1},
            startKey: {"": 12},
            endKey: {"": 12},
            endKeyInclusive: true,
            direction: 1
        }
    }
};

// Intersect foo==1 with bar==1 with baz==12.
andix1ix2 = {
    andSorted: {args: {nodes: [ixscan1, ixscan2, ixscan3]}}
};
res = db.runCommand({stageDebug: {collection: collname, plan: andix1ix2}});
printjson(res);
assert.eq(res.ok, 1);
assert.eq(res.results.length, N);

// Might as well make sure that hashed does the same thing.
andix1ix2hash = {
    andHash: {args: {nodes: [ixscan1, ixscan2, ixscan3]}}
};
res = db.runCommand({stageDebug: {collection: collname, plan: andix1ix2hash}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, N);
