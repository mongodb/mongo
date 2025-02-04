// @tags: [requires_multi_updates, requires_non_retryable_writes]

let t = db[jsTestName()];
t.drop();

for (let i = 0; i < 1000; i++) {
    assert.commandWorked(t.insert({_id: i, k: i % 12, v: "v" + i % 12}));
}

assert.commandWorked(t.createIndex({k: 1}));

assert.eq(84, t.count({k: 2, v: "v2"}), "A0");

assert.commandWorked(t.update({k: 2}, {$set: {v: "two v2"}}, false, true));

assert.eq(0, t.count({k: 2, v: "v2"}), "A1");
assert.eq(84, t.count({k: 2, v: "two v2"}), "A2");
