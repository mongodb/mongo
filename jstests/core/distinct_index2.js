t = db.distinct_index2;
t.drop();

t.ensureIndex({a: 1, b: 1});
t.ensureIndex({c: 1});

// Uniformly distributed dataset.
// If we use a randomly generated dataset, we might not
// generate all the distinct values in the range [0, 10).
for (var a = 0; a < 10; a++) {
    for (var b = 0; b < 10; b++) {
        for (var c = 0; c < 10; c++) {
            t.insert({a: a, b: b, c: c});
        }
    }
}

correct = [];
for (i = 0; i < 10; i++)
    correct.push(i);

function check(field) {
    res = t.distinct(field);
    res = res.sort();
    assert.eq(correct, res, "check: " + field);

    if (field != "a") {
        res = t.distinct(field, {a: 1});
        res = res.sort();
        assert.eq(correct, res, "check 2: " + field);
    }
}

check("a");
check("b");
check("c");

// hashed index should produce same results.
t.dropIndexes();
t.ensureIndex({a: "hashed"});
check("a");
