/**
 * Test the default behavior of window top/bottom functions when no documents fall in the window.
 * These are regression tests for: SERVER-109868.
 *
 * @tags: [requires_fcv_83,
 * ]
 */
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({_id: 0, ticker: "MDB", price: 1000, ts: new Date()}));

// Over a non-existent field.
let results = coll
    .aggregate([
        {
            $setWindowFields: {
                sortBy: {ts: 1},
                output: {
                    defaultTop: {
                        $top: {output: "$noField", sortBy: {price: 1}},
                        window: {documents: ["unbounded", "current"]},
                    },
                    defaultBottom: {
                        $bottom: {output: "$noField", sortBy: {price: 1}},
                        window: {documents: ["unbounded", "current"]},
                    },
                },
            },
        },
    ])
    .toArray();
assert(null === results[0].defaultTop, "Expected defaultTop to be null: " + tojson(results));
assert(null === results[0].defaultBottom, "Expected defaultBottom to be null: " + tojson(results));

// Over a window with no documents.
results = coll
    .aggregate([
        {
            $setWindowFields: {
                sortBy: {ts: 1},
                output: {
                    defaultTop: {
                        $top: {output: "$price", sortBy: {price: 1}},
                        window: {documents: ["unbounded", -1]},
                    },
                    defaultBottom: {
                        $bottom: {output: "$price", sortBy: {price: 1}},
                        window: {documents: ["unbounded", -1]},
                    },
                },
            },
        },
    ])
    .toArray();
assert(null === results[0].defaultTop, "Expected defaultTop to be null: " + tojson(results));
assert(null === results[0].defaultBottom, "Expected defaultBottom to be null: " + tojson(results));
