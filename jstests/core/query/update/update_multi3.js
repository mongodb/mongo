// @tags: [requires_multi_updates, requires_non_retryable_writes]

let t = db[jsTestName()];

function test(useIndex) {
    t.drop();

    if (useIndex)
        assert.commandWorked(t.createIndex({k: 1}));

    for (let i = 0; i < 10; i++) {
        assert.commandWorked(t.save({_id: i, k: 'x', a: []}));
    }

    assert.commandWorked(t.update({k: 'x'}, {$push: {a: 'y'}}, false, true));

    t.find({k: "x"}).forEach(function(z) {
        assert.eq(["y"], z.a, "useIndex: " + useIndex);
    });
}

test(false);
test(true);
