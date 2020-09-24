/**
 * Tests following query related aspects involving collated queries and indexes:
 *  1) explode for sort query planner behavior related to the selection of MERGE_SORT stage;
 *  2) MERGE_SORT stage execution.
 * @tags: [
 *   requires_find_command,
 * ]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.explode_for_sort;

// Executes a test case that creates a collection and indexes, inserts documents, issues a find
// query on a collection and compares the results with the expected collection.
function executeQueryTestCase(testCase) {
    jsTestLog(tojson(testCase));

    // Drop the test database.
    assert.commandWorked(testDB.dropDatabase());

    // Create a collection.
    const collectionOptions = {};
    if (testCase.collectionCollation !== undefined) {
        collectionOptions.collation = testCase.collectionCollation;
    }
    assert.commandWorked(testDB.createCollection(coll.getName(), collectionOptions));

    // Create index(es).
    if (testCase.indexes !== undefined) {
        assert.commandWorked(coll.createIndexes(testCase.indexes, testCase.indexOptions));
    }

    // Insert some documents into the collection.
    assert.commandWorked(coll.insert(testCase.inputDocuments));

    // Run a find query with optionally specified collation and projection.
    let projection = {};
    if (testCase.projection !== undefined) {
        projection = testCase.projection;
    }
    let cursor = coll.find(testCase.filter, projection).sort(testCase.sort);
    if (testCase.findCollation !== undefined) {
        cursor = cursor.collation(testCase.findCollation);
    }
    const actualResults = cursor.toArray();

    // Compare results to expected.
    assert.eq(actualResults, testCase.expectedResults);
}

const standardInputDocuments =
    [{_id: 0, a: 0, b: "CC"}, {_id: 1, a: 0, b: "AA"}, {_id: 2, a: 0, b: "bb"}];

const testCases = [
    {
        // Verifies that a non-collatable point-query on the prefix of the index key together with a
        // sort on a suffix of the index key returns correct results when the index is a compound
        // index with a non-simple collation and the query does not have an explicit collation.
        indexes: [{a: 1, b: 1}],
        indexOptions: {collation: {locale: "en_US", strength: 1}},
        filter: {a: 0},
        sort: {b: 1},
        inputDocuments: standardInputDocuments,
        expectedResults: [{_id: 1, a: 0, b: "AA"}, {_id: 0, a: 0, b: "CC"}, {_id: 2, a: 0, b: "bb"}]
    },
    {
        // Verifies that a non-collatable point-query on the prefix of the index key together with a
        // sort on a suffix of the index key returns correct results when the index is a compound
        // index with a non-simple collation  and the query explicitly specifies the simple
        // collation.
        indexes: [{a: 1, b: 1}],
        indexOptions: {collation: {locale: "en_US", strength: 1}},
        filter: {a: 0},
        sort: {b: 1},
        findCollation: {locale: "simple"},
        inputDocuments: standardInputDocuments,
        expectedResults: [{_id: 1, a: 0, b: "AA"}, {_id: 0, a: 0, b: "CC"}, {_id: 2, a: 0, b: "bb"}]
    },
    {
        // Verifies that a non-collatable point-query on the prefix of the index key together with a
        // sort on a suffix of the index key returns correct results when the index is a compound
        // index with a simple collation and the query explicitly specifies a non-simple collation.
        indexes: [{a: 1, b: 1}],
        filter: {a: 0},
        sort: {b: 1},
        findCollation: {locale: "en_US", strength: 1},
        inputDocuments: standardInputDocuments,
        expectedResults: [{_id: 1, a: 0, b: "AA"}, {_id: 2, a: 0, b: "bb"}, {_id: 0, a: 0, b: "CC"}]
    },
    {
        // Verifies that a non-collatable point-query on the prefix of the index key together with a
        // sort on a suffix of the index key returns correct results when the index is a compound
        // index with a simple collation and the query explicitly specifies a non-simple collation.
        indexes: [{a: 1, b: 1}],
        indexOptions: {collation: {locale: "simple"}},
        filter: {a: 0},
        sort: {b: 1},
        findCollation: {locale: "en_US", strength: 1},
        inputDocuments: standardInputDocuments,
        expectedResults: [{_id: 1, a: 0, b: "AA"}, {_id: 2, a: 0, b: "bb"}, {_id: 0, a: 0, b: "CC"}]
    },
    {
        // Verifies that a non-collatable point-query on the prefix of the index key together with a
        // sort on a suffix of the index key returns correct results when the index is a compound
        // index with a non-simple collation that is different from the query's.
        indexes: [{a: 1, b: 1}],
        indexOptions: {collation: {locale: "en_US", strength: 5}},
        filter: {a: 0},
        sort: {b: 1},
        findCollation: {locale: "en_US", strength: 1},
        inputDocuments: standardInputDocuments,
        expectedResults: [{_id: 1, a: 0, b: "AA"}, {_id: 2, a: 0, b: "bb"}, {_id: 0, a: 0, b: "CC"}]
    },
    {
        // Verifies that a non-collatable point-query on the prefix of the index key, a collatable
        // range-query on the suffix, and a sort on the suffix of the index key returns correct
        // results when the index is a compound index with a non-simple collation and the query does
        // not have an explicit collation.
        indexes: [{a: 1, b: 1}],
        indexOptions: {collation: {locale: "en_US", strength: 1}},
        filter: {a: 0, b: {$gte: 'A', $lt: 'D'}},
        sort: {b: 1},
        inputDocuments: standardInputDocuments,
        expectedResults: [{_id: 1, a: 0, b: "AA"}, {_id: 0, a: 0, b: "CC"}]
    },
    {
        // Verifies that a non-collatable point-query on the prefix of the index key, a collatable
        // range-query and a sort on a prefix of a suffix of the index key returns correct results
        // when the index is a compound index with a non-simple collation and the query does not
        // have an explicit collation.
        indexes: [{a: 1, b: 1, c: 1}],
        indexOptions: {collation: {locale: "en_US", strength: 1}},
        filter: {a: 0, b: {$gte: 'A', $lt: 'D'}},
        sort: {b: 1},
        inputDocuments: standardInputDocuments,
        expectedResults: [{_id: 1, a: 0, b: "AA"}, {_id: 0, a: 0, b: "CC"}]
    },
    {
        // Verifies that a non-collatable multi-point query on the prefix of the index key, a
        // collatable range-query on the suffix, and a sort on the suffix of the index key returns
        // correct results when the index is a compound index with a non-simple collation and the
        // query does not have an explicit collation.
        indexes: [{a: 1, b: 1}],
        indexOptions: {collation: {locale: "en_US", strength: 1}},
        filter: {a: {$in: [0, 2]}, b: {$gte: 'A', $lt: 'D'}},
        sort: {b: 1},
        inputDocuments: [
            {_id: 0, a: 0, b: "CC"},
            {_id: 1, a: 0, b: "AA"},
            {_id: 2, a: 0, b: "bb"},
            {_id: 3, a: 2, b: "BB"}
        ],
        expectedResults: [{_id: 1, a: 0, b: "AA"}, {_id: 3, a: 2, b: "BB"}, {_id: 0, a: 0, b: "CC"}]
    },
    {
        // Verifies that a non-collatable multi-point query on the prefix of the index key, a
        // non-collatable range-query on the suffix, and a sort on the suffix of the index key
        // returns correct results when the index is a compound index with a non-simple collation
        // and the query does not have an explicit collation.
        indexes: [{a: 1, b: 1}],
        indexOptions: {collation: {locale: "en_US", strength: 1}},
        filter: {a: {$in: [0, 2]}, b: {$gte: 0, $lt: 10}},
        sort: {b: 1},
        inputDocuments: [
            {_id: 0, a: 0, b: 6},
            {_id: 1, a: 0, b: 10},
            {_id: 2, a: 0, b: "bb"},
            {_id: 3, a: 2, b: 5},
            {_id: 4, a: 2, b: 4}
        ],
        expectedResults: [{_id: 4, a: 2, b: 4}, {_id: 3, a: 2, b: 5}, {_id: 0, a: 0, b: 6}]
    },
    {
        // Verifies that a non-collatable multi-point query on the prefix of the index key, a
        // non-collatable range-query on the suffix, and a sort on the suffix of the index key
        // returns correct results when the index is a compound index with a simple collation
        // and the query explicitly specifies a non-simple collation.
        indexes: [{a: 1, b: 1}],
        indexOptions: {collation: {locale: "simple"}},
        filter: {a: {$in: [0, 2]}, b: {$gte: 0, $lt: 10}},
        sort: {b: 1},
        findCollation: {locale: "en_US", strength: 1},
        inputDocuments: [
            {_id: 0, a: 0, b: 6},
            {_id: 1, a: 0, b: 10},
            {_id: 2, a: 0, b: "bb"},
            {_id: 3, a: 2, b: 5},
            {_id: 4, a: 2, b: 4}
        ],
        expectedResults: [{_id: 4, a: 2, b: 4}, {_id: 3, a: 2, b: 5}, {_id: 0, a: 0, b: 6}]
    },
    {
        // Verifies that a non-collatable point-query on the prefix of the index key, a
        // non-collatable range-query on the suffix, and a sort on the suffix of the index key
        // returns correct results when the index is a compound index with a non-simple collation
        // and the query does not have an explicit collation.
        indexes: [{a: 1, b: 1}],
        indexOptions: {collation: {locale: "en_US", strength: 1}},
        filter: {a: 0, b: {$gte: 0, $lt: 10}},
        sort: {b: 1},
        inputDocuments: [
            {_id: 0, a: 0, b: 6},
            {_id: 1, a: 0, b: 5},
            {_id: 2, a: 0, b: "bb"},
            {_id: 3, a: 0, b: 4}
        ],
        expectedResults: [{_id: 3, a: 0, b: 4}, {_id: 1, a: 0, b: 5}, {_id: 0, a: 0, b: 6}]
    },
    {
        // Verifies that a collatable multi-point query on the prefix of the index key, and a sort
        // on the suffix of the index key returns correct results when the index is a compound index
        // with a non-simple collation and the query has a collation specified matching collation of
        // the index.
        indexes: [{a: 1, b: 1}],
        indexOptions: {collation: {locale: "en", strength: 2}},
        filter: {a: {$in: ["1", "2"]}},
        sort: {b: 1},
        findCollation: {locale: "en", strength: 2},
        inputDocuments: [
            {_id: 0, a: "1", b: "a"},
            {_id: 1, a: "1", b: "c"},
            {_id: 2, a: "2", b: "b"},
            {_id: 3, a: "2", b: "d"}
        ],
        expectedResults: [
            {_id: 0, a: "1", b: "a"},
            {_id: 2, a: "2", b: "b"},
            {_id: 1, a: "1", b: "c"},
            {_id: 3, a: "2", b: "d"}
        ]
    },
    {
        // Verifies that a collatable multi-point query on the prefix of the index key, and a sort
        // on a prefix of a suffix of the index key returns correct results when the index is a
        // compound index with a non-simple collation and the query has a collation specified
        // matching collation of the index.
        indexes: [{a: 1, b: 1, c: 1, d: 1}],
        indexOptions: {collation: {locale: "en", strength: 2}},
        filter: {a: {$in: ["1", "2"]}, b: "1"},
        sort: {c: 1},
        findCollation: {locale: "en", strength: 2},
        inputDocuments: [
            {_id: 0, a: "1", b: "1", c: "a"},
            {_id: 1, a: "1", b: "1", c: "c"},
            {_id: 2, a: "2", b: "1", c: "b"},
            {_id: 3, a: "2", b: "1", c: "u", d: "a"}
        ],
        expectedResults: [
            {_id: 0, a: "1", b: "1", c: "a"},
            {_id: 2, a: "2", b: "1", c: "b"},
            {_id: 1, a: "1", b: "1", c: "c"},
            {_id: 3, a: "2", b: "1", c: "u", d: "a"}
        ]
    },
    {
        // Verifies that a collatable $or query on the prefixes of the keys of 3 indexes, and a sort
        // on a suffix of the keys of indexes returns correct results when the indexes are compound
        // indexes with a non-simple collation and the query has a collation specified matching
        // collation of the indexes. Also, the second/third operands to $or queries on fields
        // 'e'/'g' that are not covered by the indexes therefore triggers addition of a FETCH stage
        // between MERGE_SORT and IXSCAN. This tests comparison of index versus fetched document
        // provided sort keys. In addition to that, some documents have objects as sort attribute
        // values.
        indexes: [{a: 1, b: 1, c: 1}, {d: 1, c: 1}, {f: 1, c: 1}],
        indexOptions: {collation: {locale: "en", strength: 2}},
        filter: {$or: [{a: {$in: ["1", "2"]}, b: "1"}, {d: "3", e: "3"}, {f: "4", g: "3"}]},
        sort: {c: 1},
        findCollation: {locale: "en", strength: 2},
        inputDocuments: [
            {_id: 0, a: "1", b: "1", c: "a"},
            {_id: 1, a: "1", b: "1", c: "d"},
            {_id: 2, a: "2", b: "1", c: "b"},
            {_id: 3, a: "2", b: "1", c: "e"},
            {_id: 6, a: "2", b: "1", c: {a: "B"}},
            {_id: 4, d: "3", e: "3", c: "c"},
            {_id: 5, d: "3", e: "3", c: "f"},
            {_id: 7, d: "3", e: "3", c: {a: "A"}},
            {_id: 8, d: "3", e: "3", c: {a: "C"}},
            {_id: 9, f: "4", g: "3", c: "g"},
        ],
        expectedResults: [
            {_id: 0, a: "1", b: "1", c: "a"},
            {_id: 2, a: "2", b: "1", c: "b"},
            {_id: 4, d: "3", e: "3", c: "c"},
            {_id: 1, a: "1", b: "1", c: "d"},
            {_id: 3, a: "2", b: "1", c: "e"},
            {_id: 5, d: "3", e: "3", c: "f"},
            {_id: 9, f: "4", g: "3", c: "g"},
            {_id: 7, d: "3", e: "3", c: {a: "A"}},
            {_id: 6, a: "2", b: "1", c: {a: "B"}},
            {_id: 8, d: "3", e: "3", c: {a: "C"}},
        ]
    },
    {
        // Verifies that a multi-point query on the prefix of the index key, and a sort on the
        // suffix of the index key returns correct results when the collection has a non-simple
        // collation specified and the index is a compound index.
        collectionCollation: {locale: "en", strength: 2},
        indexes: [{a: 1, b: 1, c: 1, d: 1}],
        filter: {a: {$in: ["1", "2"]}, b: "1"},
        sort: {c: 1},
        inputDocuments: [
            {_id: 0, a: "1", b: "1", c: "a"},
            {_id: 1, a: "1", b: "1", c: "c"},
            {_id: 2, a: "2", b: "1", c: "b"},
            {_id: 3, a: "2", b: "1", c: "d"}
        ],
        expectedResults: [
            {_id: 0, a: "1", b: "1", c: "a"},
            {_id: 2, a: "2", b: "1", c: "b"},
            {_id: 1, a: "1", b: "1", c: "c"},
            {_id: 3, a: "2", b: "1", c: "d"}
        ]
    },
    {
        // Verifies that a multi-point query on the prefix of the index key, and a sort on the
        // suffix of the index key returns correct results when the collection has no collation
        // specified and the index is a compound index.
        indexes: [{a: 1, b: 1, c: 1, d: 1}],
        filter: {a: {$in: ["1", "2"]}, b: "1"},
        sort: {c: 1},
        inputDocuments: [
            {_id: 0, a: "1", b: "1", c: "a"},
            {_id: 1, a: "1", b: "1", c: "c"},
            {_id: 2, a: "2", b: "1", c: "b"},
            {_id: 3, a: "2", b: "1", c: "d"}
        ],
        expectedResults: [
            {_id: 0, a: "1", b: "1", c: "a"},
            {_id: 2, a: "2", b: "1", c: "b"},
            {_id: 1, a: "1", b: "1", c: "c"},
            {_id: 3, a: "2", b: "1", c: "d"}
        ]
    },
    {
        // Verifies that an $or query on the prefixes of index keys, and a sort on the suffix of the
        // index keys returns correct results when the collection and the query have the same
        // non-simple collation specified.
        indexes: [{a: 1, c: 1}, {b: 1, c: 1}],
        indexOptions: {collation: {locale: "en", strength: 2}},
        filter: {$or: [{a: "1"}, {b: "2"}]},
        sort: {c: 1},
        findCollation: {locale: "en", strength: 2},
        inputDocuments: [
            {_id: 0, a: "1", c: "a"},
            {_id: 1, a: "1", c: "c"},
            {_id: 2, b: "2", c: "b"},
            {_id: 3, b: "2", c: "d"}
        ],
        expectedResults: [
            {_id: 0, a: "1", c: "a"},
            {_id: 2, b: "2", c: "b"},
            {_id: 1, a: "1", c: "c"},
            {_id: 3, b: "2", c: "d"}
        ]
    },
    {
        // Verifies that an $or query on the prefixes of index keys, and a sort on the suffix of the
        // index keys returns correct results when the collection and the query have the same
        // non-simple collation specified and one $or operand requires a FETCH.
        indexes: [{a: 1, c: 1}, {b: 1, c: 1}],
        indexOptions: {collation: {locale: "en", strength: 2}},
        filter: {$or: [{a: "1"}, {b: "2", d: "3"}]},
        sort: {c: 1},
        findCollation: {locale: "en", strength: 2},
        inputDocuments: [
            {_id: 0, a: "1", c: "a"},
            {_id: 1, a: "1", c: "c"},
            {_id: 2, b: "2", c: "b", d: "3"},
            {_id: 3, b: "2", c: "d", d: "3"}
        ],
        expectedResults: [
            {_id: 0, a: "1", c: "a"},
            {_id: 2, b: "2", c: "b", d: "3"},
            {_id: 1, a: "1", c: "c"},
            {_id: 3, b: "2", c: "d", d: "3"}
        ]
    },
    {
        // Verifies that a non-collatable multi-point query on the prefix of the index key, and a
        // collatable sort on the suffix of the index key returns correct results when the index is
        // a compound index with a non-simple collation and the query has a collation specified
        // matching collation of the index.
        indexes: [{a: 1, b: 1}],
        indexOptions: {collation: {locale: "en", strength: 2}},
        filter: {a: {$in: [1, 2]}},
        sort: {b: 1},
        findCollation: {locale: "en", strength: 2},
        inputDocuments: [
            {_id: 0, a: 1, b: "a"},
            {_id: 1, a: 1, b: "c"},
            {_id: 2, a: 2, b: "b"},
            {_id: 3, a: 2, b: "d"}
        ],
        expectedResults: [
            {_id: 0, a: 1, b: "a"},
            {_id: 2, a: 2, b: "b"},
            {_id: 1, a: 1, b: "c"},
            {_id: 3, a: 2, b: "d"}
        ]
    },
    {
        // Verifies that a non-collatable $or query on the prefixes of the index keys, and a sort on
        // suffixes of the index keys returns correct results when the index is a compound index
        // with a non-simple collation that is different from the collation of the query.
        indexes: [{a: 1, c: 1}, {b: 1, c: 1}],
        indexOptions: {collation: {locale: "fr"}},
        filter: {$or: [{a: 1, c: 1}, {b: 2, c: 2}, {b: 2, c: 3}, {b: 2, c: 4}]},
        sort: {c: 1},
        findCollation: {locale: "en", strength: 2},
        inputDocuments: [
            {_id: 4, b: 2, c: 4},
            {_id: 2, b: 2, c: 2},
            {_id: 0, a: 1, c: 1},
            {_id: 3, b: 2, c: 3},
        ],
        expectedResults: [
            {_id: 0, a: 1, c: 1},
            {_id: 2, b: 2, c: 2},
            {_id: 3, b: 2, c: 3},
            {_id: 4, b: 2, c: 4},
        ]
    },
    {
        // Verifies that a fully-index-covered non-collatable multi-point query on a prefix of an
        // index key, and a sort on a suffix of an index key returns correct results when the index
        // is a compound index.
        indexes: [{a: 1, c: 1}],
        filter: {a: {$in: [1, 2]}},
        projection: {_id: 0, a: 1, c: 1},
        sort: {c: 1},
        inputDocuments: [
            {_id: 0, a: 1, c: 5},
            {_id: 1, a: 1, c: 3},
            {_id: 2, a: 2, c: 1},
            {_id: 3, a: 2, c: 4},
        ],
        expectedResults: [
            {a: 2, c: 1},
            {a: 1, c: 3},
            {a: 2, c: 4},
            {a: 1, c: 5},
        ]
    },
    {
        // Verifies that a non-collatable multi-point query on a prefix of an index key, and a
        // collatable sort on a suffix of an index key returns correct results when the index is a
        // compound index with a collation specified that matches a collation of the query. The
        // query would be eligible to be covered by the index due to a projection, but requires a
        // FETCH because index bounds include strings that are encoded as collated keys.
        indexes: [{a: 1, c: 1}],
        indexOptions: {collation: {locale: "en", strength: 2}},
        filter: {a: {$in: [1, 2]}},
        projection: {_id: 0, a: 1, c: 1},
        sort: {c: 1},
        findCollation: {locale: "en", strength: 2},
        inputDocuments: [
            {_id: 0, a: 1, c: "a"},
            {_id: 1, a: 1, c: "c"},
            {_id: 2, a: 2, c: "b"},
            {_id: 3, a: 2, c: "d"},
        ],
        expectedResults: [
            {a: 1, c: "a"},
            {a: 2, c: "b"},
            {a: 1, c: "c"},
            {a: 2, c: "d"},
        ]
    },
];

testCases.forEach(executeQueryTestCase);
}());