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

// Test that stageDebug fails if neither the keyPattern nor the index name are present.
assert.commandFailed(db.runCommand({
    stageDebug: {
        collection: collname,
        plan: {
            ixscan: {
                args: {
                    startKey: {
                        "": 20,
                        endKey: {},
                        startKeyInclusive: true,
                        endKeyInclusive: true,
                        direction: -1
                    }
                }
            }
        }
    }
}));

// foo <= 20
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
            startKeyInclusive: true,
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
            startKeyInclusive: true,
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
            startKeyInclusive: true,
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
            startKeyInclusive: true,
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
            startKeyInclusive: true,
            endKeyInclusive: true,
            direction: 1
        },
        filter: {bar: 25}
    }
};
res = db.runCommand({stageDebug: {collection: collname, plan: ixscan1}});
assert.eq(res.ok, 0);

t.drop();
assert.commandWorked(t.createIndex(
    {a: 1}, {name: "numeric", collation: {locale: "en_US", strength: 3, numericOrdering: true}}));
assert.commandWorked(
    t.createIndex({a: 1}, {name: "s3", collation: {locale: "en_US", strength: 3}}));

// Stage debug does not allow ixscan with ambiguous key patterns.

var ixscanAmbiguous = {
    ixscan: {
        args: {
            keyPattern: {a: 1},
            startKey: {a: 1},
            endKey: {a: 2},
            startKeyInclusive: true,
            endKeyInclusive: true,
            direction: 1
        },
        filter: {}
    }
};

assert.commandFailed(db.runCommand({stageDebug: {collection: collname, plan: ixscanAmbiguous}}));

// Stage debug allows selecting indexes by name.
var ixscanName = {
    ixscan: {
        args: {
            name: "numeric",
            startKey: {a: ""},
            endKey: {a: {}},  // All strings
            startKeyInclusive: true,
            endKeyInclusive: false,
            direction: 1
        },
        filter: {}
    }
};

assert.writeOK(t.insert([{a: "1234"}, {a: "124"}]));
var res = db.runCommand({stageDebug: {collection: collname, plan: ixscanName}});
assert.commandWorked(res);
assert.eq(res.results.map((doc) => doc.a), ["124", "1234"]);
