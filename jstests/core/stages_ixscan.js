// Test basic query stage index scan functionality.
t = db.stages_ixscan;
t.drop();
var collname = "stages_ixscan";

var N = 50;
for (var i = 0; i < N; ++i) {
    t.insert({foo: i, bar: N - i, baz: i});
}

t.ensureIndex({foo: 1});
t.ensureIndex({foo: 1, baz: 1});

// foo <= 20
ixscan1 = {
    ixscan: {
        args: {
            keyPattern: {foo: 1},
            startKey: {"": 20},
            endKey: {},
            endKeyInclusive: true,
            direction: -1
        }
    }
};
res = db.runCommand({stageDebug: {collection: collname, plan: ixscan1}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 21);

// 20 <= foo < 30
ixscan1 = {
    ixscan: {
        args: {
            keyPattern: {foo: 1},
            startKey: {"": 20},
            endKey: {"": 30},
            endKeyInclusive: false,
            direction: 1
        }
    }
};
res = db.runCommand({stageDebug: {collection: collname, plan: ixscan1}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 10);

// 20 <= foo <= 30
ixscan1 = {
    ixscan: {
        args: {
            keyPattern: {foo: 1},
            startKey: {"": 20},
            endKey: {"": 30},
            endKeyInclusive: true,
            direction: 1
        }
    }
};
res = db.runCommand({stageDebug: {collection: collname, plan: ixscan1}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 11);

// 20 <= foo <= 30
// foo == 25
ixscan1 = {
    ixscan: {
        args: {
            keyPattern: {foo: 1},
            startKey: {"": 20},
            endKey: {"": 30},
            endKeyInclusive: true,
            direction: 1
        },
        filter: {foo: 25}
    }
};
res = db.runCommand({stageDebug: {collection: collname, plan: ixscan1}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 1);

// 20 <= foo <= 30
// baz == 25 (in index so we can match against it.)
ixscan1 = {
    ixscan: {
        args: {
            keyPattern: {foo: 1, baz: 1},
            startKey: {foo: 20, baz: MinKey},
            endKey: {foo: 30, baz: MaxKey},
            endKeyInclusive: true,
            direction: 1
        },
        filter: {baz: 25}
    }
};
res = db.runCommand({stageDebug: {collection: collname, plan: ixscan1}});
assert.eq(res.ok, 1);
assert.eq(res.results.length, 1);

// 20 <= foo <= 30
// bar == 25 (not covered, should error.)
ixscan1 = {
    ixscan: {
        args: {
            keyPattern: {foo: 1, baz: 1},
            startKey: {foo: 20, baz: MinKey},
            endKey: {foo: 30, baz: MaxKey},
            endKeyInclusive: true,
            direction: 1
        },
        filter: {bar: 25}
    }
};
res = db.runCommand({stageDebug: {collection: collname, plan: ixscan1}});
assert.eq(res.ok, 0);
