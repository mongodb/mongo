// Ensures that end to end results are correct after matches are swapped before the group operator.
// We test specifically the indirect version of this where we're not filtering on _id directly but
// instead on an aggregate which evaluates to a key in the _id.
// @tags: []

let coll = db[jsTestName()];

coll.drop();

for (let i = 0; i < 10; i++) {
    coll.insert({a: i, b: 10 + i, c: 20 + i});
}

let testScenario =
    (query, expectedResults) => {
        let results = coll.aggregate(query).toArray();
        assert.eq(expectedResults, results.length, "Found results: " + JSON.stringify(results));
    }

let operators = ["$first", "$last", "$max", "$min"];
for (let operator of operators) {
    testScenario([{$group: {_id: "$a", aa: {[operator]: "$a"}}}, {$match: {aa: {$lt: 5}}}], 5);
}

// Test behavior for compound id.
for (let operator of operators) {
    for (let operator2 of operators) {
        testScenario(
            [
                {
                    $group:
                        {_id: {a: "$a", b: "$b"}, aa: {[operator]: "$a"}, bb: {[operator2]: "$b"}}
                },
                {$match: {aa: {$lt: 6}, bb: {$gt: 14}}}
            ],
            1);
    }
}

coll.drop();

for (let i = 0; i < 10; i++) {
    coll.insert({a: i, b: 10 + i, c: 20 + i});
    coll.insert({a: i, b: 10 + i, c: 20 + i});
}

// Test behavior with non-optimization eligible operator $firstN which is in the compound id.
for (let operator of operators) {
    for (let operator2 of operators) {
        testScenario(
            [
                {
                    $group: {
                        _id: {a: "$a", b: "$b", c: "$c"},
                        aa: {[operator]: "$a"},
                        bb: {[operator2]: "$b"},
                        cc: {$firstN: {input: "$c", n: 1}}
                    }
                },
                {$match: {aa: {$lt: 5}, bb: {$lt: 15}, cc: [24]}}
            ],
            1);
    }
}

coll.drop();

for (let i = 0; i < 10; i++) {
    coll.insert({a: i, b: 10 + i, c: i % 2});
    coll.insert({a: i, b: 10 + 1 + i, c: (i + 1) % 2});
}

// Test the behavior with non-optimization eligible field c.
// If the c match expression was pushed before this then we would get the average of b to be 15.
// This tests that non-_id match expressions don't get pushed before.
for (let operator of operators) {
    for (let operator2 of operators) {
        testScenario(
            [
                {$sort: {c: 1}},
                {$group: {_id: "$a", aa: {[operator]: "$a"}, bb: {$avg: "$b"}, cc: {$first: "$c"}}},
                {$match: {aa: 5, bb: 15.5, cc: 0}}
            ],
            1);
    }
}
