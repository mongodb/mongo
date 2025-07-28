export const TestCases = [
    {
        query: [
            {$match: {a: 1}},
            {
                $group: {
                    '_id': '$m.m1',
                }
            }
        ],
        index: {a: 1, m: 'hashed', 'm.m1': 1},
        indexName: "testIndexName",
        docs: [{_id: 1, a: 1, m: {m1: 2}}],
        results: [{"_id": 2}],
    },
    {
        query: [
            {$match: {'a.a1': 1}},
            {
                $group: {
                    '_id': '$m.m1',
                    // Paths in the accumulators also do not need
                    // getFields and array traversals when reading from a
                    // covered hashed index scan.
                    total: {$sum: "$a.a1"},
                }
            }
        ],
        // 'a.a1' in the hashed index guarantees 'a.a1' is not an array.
        index: {'a.a1': 1, m: 'hashed', 'm.m1': 1},
        indexName: "testIndexName",
        docs: [{_id: 1, a: {a1: 1}, m: {m1: 2}}],
        results: [{"_id": 2, "total": 1}],
    },
    {
        query: [{"$project": {"t": "$m.m2"}}],
        index: {"_id": 1, "m.m2": 1, "m": "hashed"},
        indexName: "mHashed",
        docs: [{"_id": 0, "m": {"m1": 0, "m2": 0}}],
        results: [{"_id": 0, "t": 0}],
    },
    {
        query: [{"$project": {"t": "$a.b"}}, {$group: {_id: null, ab: {$first: "$t"}}}],
        index: {"_id": 1, "a.b": 1, "a": "hashed"},
        indexName: "aHashed",
        docs: [{"_id": 0, "a": {"b": 0}}],
        results: [{"_id": null, "ab": 0}],
    },
];
