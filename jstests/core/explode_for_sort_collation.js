/**
 * Tests explode for sort query planner behavior with collated queries and indexes. This is a test
 * for SERVER-48993.
 * @tags: [requires_find_command]
 */
(function() {
    "use strict";

    const testDB = db.getSiblingDB(jsTestName());

    // Drop the test database.
    assert.commandWorked(testDB.dropDatabase());

    const coll = testDB.explode_for_sort;

    // Executes a test case that creates an index, inserts documents, issues a find query on a
    // collection and compares the results with the expected collection.
    function executeQueryTestCase(testCase) {
        jsTestLog(tojson(testCase));

        // Create a collection.
        coll.drop();
        assert.commandWorked(testDB.createCollection(coll.getName()));

        // Create an index.
        assert.commandWorked(coll.createIndex(testCase.indexKeyPattern, testCase.indexOptions));

        // Insert some documents into the collection.
        assert.commandWorked(coll.insert(testCase.inputDocuments));

        // Run a find query with optionally specified collation.
        let cursor = coll.find(testCase.filter).sort(testCase.sort);
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
          // Verifies that a non-collatable point-query on the prefix of the index key together with
          // a
          // sort on a suffix of the index key returns correct results when the index is a compound
          // index with a non-simple collation and the query does not have an explicit collation.
          indexKeyPattern: {a: 1, b: 1},
          indexOptions: {collation: {locale: "en_US", strength: 1}},
          filter: {a: 0},
          sort: {b: 1},
          inputDocuments: standardInputDocuments,
          expectedResults:
              [{_id: 1, a: 0, b: "AA"}, {_id: 0, a: 0, b: "CC"}, {_id: 2, a: 0, b: "bb"}]
        },
        {
          // Verifies that a non-collatable point-query on the prefix of the index key together with
          // a
          // sort on a suffix of the index key returns correct results when the index is a compound
          // index with a non-simple collation  and the query explicitly specifies the simple
          // collation.
          indexKeyPattern: {a: 1, b: 1},
          indexOptions: {collation: {locale: "en_US", strength: 1}},
          filter: {a: 0},
          sort: {b: 1},
          findCollation: {locale: "simple"},
          inputDocuments: standardInputDocuments,
          expectedResults:
              [{_id: 1, a: 0, b: "AA"}, {_id: 0, a: 0, b: "CC"}, {_id: 2, a: 0, b: "bb"}]
        },
        {
          // Verifies that a non-collatable point-query on the prefix of the index key together with
          // a
          // sort on a suffix of the index key returns correct results when the index is a compound
          // index with a simple collation and the query explicitly specifies a non-simple
          // collation.
          indexKeyPattern: {a: 1, b: 1},
          filter: {a: 0},
          sort: {b: 1},
          findCollation: {locale: "en_US", strength: 1},
          inputDocuments: standardInputDocuments,
          expectedResults:
              [{_id: 1, a: 0, b: "AA"}, {_id: 2, a: 0, b: "bb"}, {_id: 0, a: 0, b: "CC"}]
        },
        {
          // Verifies that a non-collatable point-query on the prefix of the index key together with
          // a
          // sort on a suffix of the index key returns correct results when the index is a compound
          // index with a simple collation and the query explicitly specifies a non-simple
          // collation.
          indexKeyPattern: {a: 1, b: 1},
          indexOptions: {collation: {locale: "simple"}},
          filter: {a: 0},
          sort: {b: 1},
          findCollation: {locale: "en_US", strength: 1},
          inputDocuments: standardInputDocuments,
          expectedResults:
              [{_id: 1, a: 0, b: "AA"}, {_id: 2, a: 0, b: "bb"}, {_id: 0, a: 0, b: "CC"}]
        },
        {
          // Verifies that a non-collatable point-query on the prefix of the index key together with
          // a
          // sort on a suffix of the index key returns correct results when the index is a compound
          // index with a non-simple collation that is different from the query's.
          indexKeyPattern: {a: 1, b: 1},
          indexOptions: {collation: {locale: "en_US", strength: 5}},
          filter: {a: 0},
          sort: {b: 1},
          findCollation: {locale: "en_US", strength: 1},
          inputDocuments: standardInputDocuments,
          expectedResults:
              [{_id: 1, a: 0, b: "AA"}, {_id: 2, a: 0, b: "bb"}, {_id: 0, a: 0, b: "CC"}]
        },
        {
          // Verifies that a non-collatable point-query on the prefix of the index key, a collatable
          // range-query on the suffix, and a sort on the suffix of the index key returns correct
          // results when the index is a compound index with a non-simple collation and the query
          // does
          // not have an explicit collation.
          indexKeyPattern: {a: 1, b: 1},
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
          indexKeyPattern: {a: 1, b: 1, c: 1},
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
          indexKeyPattern: {a: 1, b: 1},
          indexOptions: {collation: {locale: "en_US", strength: 1}},
          filter: {a: {$in: [0, 2]}, b: {$gte: 'A', $lt: 'D'}},
          sort: {b: 1},
          inputDocuments: [
              {_id: 0, a: 0, b: "CC"},
              {_id: 1, a: 0, b: "AA"},
              {_id: 2, a: 0, b: "bb"},
              {_id: 3, a: 2, b: "BB"}
          ],
          expectedResults:
              [{_id: 1, a: 0, b: "AA"}, {_id: 3, a: 2, b: "BB"}, {_id: 0, a: 0, b: "CC"}]
        },
        {
          // Verifies that a non-collatable multi-point query on the prefix of the index key, a
          // non-collatable range-query on the suffix, and a sort on the suffix of the index key
          // returns correct results when the index is a compound index with a non-simple collation
          // and the query does not have an explicit collation.
          indexKeyPattern: {a: 1, b: 1},
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
          indexKeyPattern: {a: 1, b: 1},
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
          indexKeyPattern: {a: 1, b: 1},
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
        }
    ];

    testCases.forEach(executeQueryTestCase);
}());