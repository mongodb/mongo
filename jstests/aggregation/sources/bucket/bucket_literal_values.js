// @tags: [assumes_no_implicit_collection_creation_after_drop]

const runTest = function (label, callback) {
    jsTestLog(`Running test: ${label}`);

    const coll = db.bucket_literal_values;

    coll.drop();
    
    try {
        callback(coll);
    } finally {
        coll.drop();
    }
};

runTest("treats default {$literal: '$color'} as the literal string", function (coll) {
    assert.commandWorked(
        coll.insert([
            {x: 5, color: "red"},
            {x: 10, color: "blue"},
        ]),
    );

    const results = coll
        .aggregate([
            {$bucket: {groupBy: "$x", boundaries: [0, 1], default: {$literal: "$color"}}},
        ])
        .toArray();
    assert.eq(1, results.length, {results});
    assert.eq({_id: "$color", count: 2}, results[0], {results});
});

runTest("treats default {$literal: '$$ROOT'} as the literal string", function (coll) {
    assert.commandWorked(
        coll.insert([
            {x: 5, color: "red"},
            {x: 10, color: "blue"},
        ]),
    );

    const results = coll
        .aggregate([
            {$bucket: {groupBy: "$x", boundaries: [0, 1], default: {$literal: "$$ROOT"}}},
        ])
        .toArray();
    assert.eq(1, results.length, {results});
    assert.eq({_id: "$$ROOT", count: 2}, results[0], {results});
});

runTest("treats boundary literal strings as literals", function (coll) {
    assert.commandWorked(
        coll.insert([
            {x: "$m", l: "$j", n: "$o"},
            {x: "$m", l: "$i", n: "$p"},
        ]),
    );

    const results = coll
        .aggregate([
            {
                $bucket: {
                    groupBy: "$x",
                    boundaries: [{$literal: "$l"}, {$literal: "$n"}],
                    default: "other",
                },
            },
        ])
        .toArray();
    assert.eq(1, results.length, {results});
    assert.eq({_id: "$l", count: 2}, results[0], {results});
});
