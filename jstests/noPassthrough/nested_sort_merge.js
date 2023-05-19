/**
 * Verifies that nested SORT_MERGE plans are handled correctly by the SBE stage builder.
 * Intended to reproduce SERVER-61496.
 */
(function() {

load("jstests/libs/analyze_plan.js");  // for 'getPlanStages'.

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

// Disable MatchExpression optimization so that we can craft simple queries that can be answered
// with a nested SORT_MERGE plan. If we allow optimization, then all of the nested $or predicates
// will be optimized away (all child predicates will be raised to children of the top level $or).
assert.commandWorked(
    db.adminCommand({configureFailPoint: "disableMatchExpressionOptimization", mode: "alwaysOn"}));

const collName = jsTestName();

const nDocs = 100;
const idxSpec = {
    a: 1,
    b: -1
};
const queries = [
    // Simple case: one top level SORT_MERGE which has two SORT_MERGE children with 2 IXSCAN
    // leaves each.
    {
        query: {
            $or: [
                {$or: [{a: 2, b: 1}, {a: 4, b: 4}, {a: 2, b: 5}]},
                {$or: [{a: 1, b: 2}, {a: 5, b: 5}]}
            ]
        },
        sortMergeCount: 3
    },

    // SORT_MERGE nested to 3 levels.
    {
        query: {
            $or: [
                {$or: [{$or: [{a: 2, b: 1}, {a: 4, b: 4}]}, {a: 3, b: 3}]},
                {$or: [{a: 1, b: 2}, {a: 5, b: 5}]}
            ]
        },
        sortMergeCount: 4
    }
];

const sortPatterns = [
    // Basic case.
    {
        sort: {a: 1, b: 1},
        cmpFn: (docOne, docTwo) =>
            docOne.a < docTwo.a || (docOne.a === docTwo.a && docOne.b <= docTwo.b)
    },

    // Verify that an index key pattern which doesn't match the order of the sort pattern can still
    // be used to satisfy the sort.
    {
        sort: {b: -1, a: 1},
        cmpFn: (docOne, docTwo) =>
            docOne.b > docTwo.b || (docOne.b === docTwo.b && docOne.a <= docTwo.a)
    }
];

const coll = db[collName];
assert.commandWorked(coll.createIndex(idxSpec));

let docs = [];
for (let i = 0; i < nDocs; ++i) {
    docs.push({
        a: i % 6,
        b: i % 7,
    });
}
assert.commandWorked(coll.insert(docs));

for (const doc of queries) {
    for (const sortPatternDoc of sortPatterns) {
        const query = doc.query;
        const sortPattern = sortPatternDoc.sort;
        const sortFn = sortPatternDoc.cmpFn;

        const expectedSortMergeCount = doc.sortMergeCount;
        const explain = coll.find(query).sort(sortPattern).explain();
        const sortMergeStages = getPlanStages(explain, "SORT_MERGE");
        assert.eq(expectedSortMergeCount,
                  sortMergeStages.length,
                  "Incorrect number of SORT_MERGE stages; explain: " + tojson(explain));

        const docs = coll.find(query).sort(sortPattern).toArray();
        for (let i = 1; i < docs.length; ++i) {
            assert(sortFn(docs[i - 1], docs[i]),
                   "Out of order results for " + tojson(query) +
                       "; sortPattern: " + tojson(sortPattern) + "; results: " + tojson(docs));
        }
    }
}
MongoRunner.stopMongod(conn);
})();
