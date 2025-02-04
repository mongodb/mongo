// This test performs {multi: true} upsert which requires specifying the full shard key.
// @tags: [assumes_unsharded_collection, requires_multi_updates, requires_non_retryable_writes]

// Test new (optional) update syntax
// SERVER-4176
let t = db[jsTestName()];

// Using a multi update

t.drop();

for (let i = 0; i < 10; i++) {
    assert.commandWorked(t.save({_id: i, k: "x", a: []}));
}

assert.commandWorked(t.update({k: "x"}, {$push: {a: "y"}}, {multi: true}));
t.find({k: "x"}).forEach(function(z) {
    assert.eq(["y"], z.a, "multi update using object arg");
});

assert(t.drop());

// Using a single update

for (let i = 0; i < 10; i++) {
    t.save({_id: i, k: "x", a: []});
}

assert.commandWorked(t.update({k: "x"}, {$push: {a: "y"}}, {multi: false}));
assert.eq(1, t.find({"a": "y"}).count(), "update using object arg");

assert(t.drop());

// Using upsert, found

for (let i = 0; i < 10; i++) {
    assert.commandWorked(t.save({_id: i, k: "x", a: []}));
}

assert.commandWorked(t.update({k: "x"}, {$push: {a: "y"}}, {upsert: true}));
assert.eq(1, t.find({"k": "x", "a": "y"}).count(), "upsert (found) using object arg");

assert(t.drop());

// Using upsert + multi, found

for (let i = 0; i < 10; i++) {
    assert.commandWorked(t.save({_id: i, k: "x", a: []}));
}

assert.commandWorked(t.update({k: "x"}, {$push: {a: "y"}}, {upsert: true, multi: true}));
t.find({k: "x"}).forEach(function(z) {
    assert.eq(["y"], z.a, "multi + upsert (found) using object arg");
});

assert(t.drop());

// Using upsert, not found

for (let i = 0; i < 10; i++) {
    assert.commandWorked(t.save({_id: i, k: "x", a: []}));
}

assert.commandWorked(t.update({k: "y"}, {$push: {a: "y"}}, {upsert: true}));
assert.eq(1, t.find({"k": "y", "a": "y"}).count(), "upsert (not found) using object arg");

assert(t.drop());

// Without upsert, found

for (let i = 0; i < 10; i++) {
    assert.commandWorked(t.save({_id: i, k: "x", a: []}));
}

assert.commandWorked(t.update({k: "x"}, {$push: {a: "y"}}, {upsert: false}));
assert.eq(1, t.find({"a": "y"}).count(), "no upsert (found) using object arg");

assert(t.drop());

// Without upsert, not found

for (let i = 0; i < 10; i++) {
    assert.commandWorked(t.save({_id: i, k: "x", a: []}));
}

assert.commandWorked(t.update({k: "y"}, {$push: {a: "y"}}, {upsert: false}));
assert.eq(0, t.find({"a": "y"}).count(), "no upsert (not found) using object arg");

assert(t.drop());
