
t = db.update_multi3;

function test(useIndex) {
    t.drop();

    if (useIndex)
        t.ensureIndex({k: 1});

    for (i = 0; i < 10; i++) {
        t.save({_id: i, k: 'x', a: []});
    }

    t.update({k: 'x'}, {$push: {a: 'y'}}, false, true);

    t.find({k: "x"}).forEach(function(z) {
        assert.eq(["y"], z.a, "useIndex: " + useIndex);
    });
}

test(false);
test(true);
