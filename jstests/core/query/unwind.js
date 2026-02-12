/**
 * Sanity test for $unwind.
 *
 * @tags: [
 *    # Aggregate is not supported in multi-document transactions.
 *    does_not_support_transactions,
 *    requires_non_retryable_writes,
 * ]
 */

// TODO (SERVER-117130): Remove the mongos pinning once the related issue is resolved.
// When a database is dropped, a stale router will report "database not found" error for
// deletes (instead of "ok") when pauseMigrationsDuringMultiUpdates is enabled.
if (TestData.pauseMigrationsDuringMultiUpdates) {
    TestData.pinToSingleMongos = true;
}

import {resultsEq} from "jstests/aggregation/extras/utils.js";
db.c.drop();

const testCases = [
    // 'a' is missing.
    {input: {_id: 0}, query: {path: "$a"}, output: []},
    {input: {_id: 0}, query: {path: "$a", preserveNullAndEmptyArrays: true}, output: [{_id: 0}]},
    {input: {_id: 0}, query: {path: "$a", includeArrayIndex: "i"}, output: []},
    {
        input: {_id: 0},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "i"},
        output: [{_id: 0, i: null}],
    },
    {
        input: {_id: 0},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "a"},
        output: [{_id: 0, a: null}],
    },

    // 'a' is null.
    {input: {_id: 0, a: null}, query: {path: "$a"}, output: []},
    {input: {_id: 0, a: null}, query: {path: "$a", preserveNullAndEmptyArrays: true}, output: [{_id: 0, a: null}]},
    {input: {_id: 0, a: null}, query: {path: "$a", includeArrayIndex: "i"}, output: []},
    {
        input: {_id: 0, a: null},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "i"},
        output: [{_id: 0, a: null, i: null}],
    },
    {
        input: {_id: 0, a: null},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "a"},
        output: [{_id: 0, a: null}],
    },

    // 'a' is empty array.
    {input: {_id: 0, a: []}, query: {path: "$a"}, output: []},
    {input: {_id: 0, a: []}, query: {path: "$a", preserveNullAndEmptyArrays: true}, output: [{_id: 0}]},
    {input: {_id: 0, a: []}, query: {path: "$a", includeArrayIndex: "i"}, output: []},
    {
        input: {_id: 0, a: []},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "i"},
        output: [{_id: 0, i: null}],
    },
    {
        input: {_id: 0, a: []},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "a"},
        output: [{_id: 0, a: null}],
    },

    // 'a' is 1.
    {input: {_id: 0, a: 1}, query: {path: "$a"}, output: [{_id: 0, a: 1}]},
    {input: {_id: 0, a: 1}, query: {path: "$a", preserveNullAndEmptyArrays: true}, output: [{_id: 0, a: 1}]},
    {input: {_id: 0, a: 1}, query: {path: "$a", includeArrayIndex: "i"}, output: [{_id: 0, a: 1, i: null}]},
    {
        input: {_id: 0, a: 1},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "i"},
        output: [{_id: 0, a: 1, i: null}],
    },
    {
        input: {_id: 0, a: 1},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "a"},
        output: [{_id: 0, a: null}],
    },

    // 'a' is empty document.
    {input: {_id: 0, a: {}}, query: {path: "$a"}, output: [{_id: 0, a: {}}]},
    {input: {_id: 0, a: {}}, query: {path: "$a", preserveNullAndEmptyArrays: true}, output: [{_id: 0, a: {}}]},
    {input: {_id: 0, a: {}}, query: {path: "$a", includeArrayIndex: "i"}, output: [{_id: 0, a: {}, i: null}]},
    {
        input: {_id: 0, a: {}},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "i"},
        output: [{_id: 0, a: {}, i: null}],
    },
    {
        input: {_id: 0, a: {}},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "a"},
        output: [{_id: 0, a: null}],
    },

    // 'a' is array containing null.
    {input: {_id: 0, a: [null]}, query: {path: "$a"}, output: [{_id: 0, a: null}]},
    {input: {_id: 0, a: [null]}, query: {path: "$a", preserveNullAndEmptyArrays: true}, output: [{_id: 0, a: null}]},
    {input: {_id: 0, a: [null]}, query: {path: "$a", includeArrayIndex: "i"}, output: [{_id: 0, a: null, i: 0}]},
    {
        input: {_id: 0, a: [null]},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "i"},
        output: [{_id: 0, a: null, i: 0}],
    },
    {
        input: {_id: 0, a: [null]},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "a"},
        output: [{_id: 0, a: 0}],
    },

    // 'a' is array containing empty array.
    {input: {_id: 0, a: [[]]}, query: {path: "$a"}, output: [{_id: 0, a: []}]},
    {input: {_id: 0, a: [[]]}, query: {path: "$a", preserveNullAndEmptyArrays: true}, output: [{_id: 0, a: []}]},
    {input: {_id: 0, a: [[]]}, query: {path: "$a", includeArrayIndex: "i"}, output: [{_id: 0, a: [], i: 0}]},
    {
        input: {_id: 0, a: [[]]},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "i"},
        output: [{_id: 0, a: [], i: 0}],
    },
    {
        input: {_id: 0, a: [[]]},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "a"},
        output: [{_id: 0, a: 0}],
    },

    // 'a' is array containing 1.
    {input: {_id: 0, a: [1]}, query: {path: "$a"}, output: [{_id: 0, a: 1}]},
    {input: {_id: 0, a: [1]}, query: {path: "$a", preserveNullAndEmptyArrays: true}, output: [{_id: 0, a: 1}]},
    {input: {_id: 0, a: [1]}, query: {path: "$a", includeArrayIndex: "i"}, output: [{_id: 0, a: 1, i: 0}]},
    {
        input: {_id: 0, a: [1]},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "i"},
        output: [{_id: 0, a: 1, i: 0}],
    },
    {
        input: {_id: 0, a: [1]},
        query: {path: "$a", preserveNullAndEmptyArrays: true, includeArrayIndex: "a"},
        output: [{_id: 0, a: 0}],
    },
];

for (const tc of testCases) {
    jsTest.log(tc);
    db.c.deleteMany({});
    db.c.insert(tc.input);
    const results = db.c.aggregate({$unwind: tc.query}).toArray();
    jsTest.log(results);
    assert(resultsEq(tc.output, results));
}
