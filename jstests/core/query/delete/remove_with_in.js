// @tags: [requires_non_retryable_writes]

let t = db[jsTestName()];
t.drop();

function getTags(n) {
    n = n || 5;
    let a = [];
    for (let i = 0; i < n; i++) {
        const v = Math.ceil(20 * Math.random());
        a.push(v);
    }

    return a;
}

for (let i = 0; i < 1000; i++) {
    assert.commandWorked(t.save({tags: getTags()}));
}

assert.commandWorked(t.createIndex({tags: 1}));

for (let i = 0; i < 200; i++) {
    for (let j = 0; j < 10; j++) {
        assert.commandWorked(t.save({tags: getTags(100)}));
    }
    const q = {tags: {$in: getTags(10)}};
    const res = assert.commandWorked(t.remove(q));
    const after = t.find(q).count();
    assert.eq(0, after, "not zero after!");
    assert.commandWorked(res);
}
