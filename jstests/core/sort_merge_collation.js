/**
 * Tests $or queries which can be answered with a SORT_MERGE stage using a non-default collation
 * with numeric ordering.
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

const numericOrdering = {
    collation: {locale: "en_US", numericOrdering: true}
};

const coll = db.sort_merge_collation;
coll.drop();

assert.commandWorked(db.createCollection(coll.getName(), numericOrdering));

assert.commandWorked(
    coll.createIndex({filterFieldA: 1, sortFieldA: 1, sortFieldB: 1}, numericOrdering));
assert.commandWorked(
    coll.createIndex({filterFieldA: 1, sortFieldA: -1, sortFieldB: -1}, numericOrdering));
assert.commandWorked(coll.createIndex({sortFieldA: 1, sortFieldB: 1}, numericOrdering));

assert.commandWorked(coll.insert([
    {filterFieldA: "1", filterFieldB: "1", sortFieldA: "18", sortFieldB: "11"},
    {filterFieldA: "2", filterFieldB: "2", sortFieldA: "2", sortFieldB: "13"},
    {filterFieldA: "3", filterFieldB: "3", sortFieldA: "13", sortFieldB: "7"},
    {filterFieldA: "4", filterFieldB: "4", sortFieldA: "7", sortFieldB: "4"},
    {filterFieldA: "5", filterFieldB: "5", sortFieldA: "14", sortFieldB: "10"},
    {filterFieldA: "1", filterFieldB: "1", sortFieldA: "4", sortFieldB: "12"},
    {filterFieldA: "2", filterFieldB: "2", sortFieldA: "11", sortFieldB: "8"},
    {filterFieldA: "3", filterFieldB: "3", sortFieldA: "9", sortFieldB: "5"},
    {filterFieldA: "4", filterFieldB: "4", sortFieldA: "10", sortFieldB: "14"},
    {filterFieldA: "5", filterFieldB: "5", sortFieldA: "1", sortFieldB: "18"},
    {filterFieldA: "1", filterFieldB: "1", sortFieldA: "18", sortFieldB: "6"},
    {filterFieldA: "2", filterFieldB: "2", sortFieldA: "2", sortFieldB: "3"},
    {filterFieldA: "3", filterFieldB: "3", sortFieldA: "13", sortFieldB: "19"},
    {filterFieldA: "4", filterFieldB: "4", sortFieldA: "7", sortFieldB: "15"},
    {filterFieldA: "5", filterFieldB: "5", sortFieldA: "14", sortFieldB: "1"},
    {filterFieldA: "1", filterFieldB: "1", sortFieldA: "4", sortFieldB: "0"},
    {filterFieldA: "2", filterFieldB: "2", sortFieldA: "11", sortFieldB: "17"},
    {filterFieldA: "3", filterFieldB: "3", sortFieldA: "9", sortFieldB: "16"},
    {filterFieldA: "4", filterFieldB: "4", sortFieldA: "10", sortFieldB: "9"},
    {filterFieldA: "5", filterFieldB: "5", sortFieldA: "1", sortFieldB: "2"},
]));

function isSorted(array, lessThanFunction) {
    for (let i = 1; i < array.length; ++i) {
        if (lessThanFunction(array[i], array[i - 1])) {
            return false;
        }
    }
    return true;
}

function runTest(sorts, filters) {
    for (let sortInfo of sorts) {
        for (let filter of filters) {
            // Verify that the sort/filter combination produces a SORT_MERGE plan.
            const explain = coll.find(filter).sort(sortInfo.sortPattern).explain("queryPlanner");
            const sortMergeStages = getPlanStages(explain, "SORT_MERGE");
            assert.gt(sortMergeStages.length, 0, explain);

            // Check that the results are in order.
            let res = coll.find(filter).sort(sortInfo.sortPattern).toArray();
            assert(isSorted(res, sortInfo.cmpFunction),
                   () => "Assertion failed for filter: " + filter + "\n" +
                       "sort pattern " + sortInfo.sortPattern);

            // Check that there are no duplicates.
            let ids = new Set();
            for (let doc of res) {
                assert(!ids.has(doc._id), () => "Duplicate _id: " + tojson(_id));
                ids.add(doc._id);
            }
        }
    }
}

const kSorts = [
    {
        sortPattern: {sortFieldA: 1},
        cmpFunction: (docA, docB) => parseInt(docA.sortFieldA) < parseInt(docB.sortFieldA)
    },
    {
        sortPattern: {sortFieldA: -1},
        cmpFunction: (docA, docB) => parseInt(docA.sortFieldA) > parseInt(docB.sortFieldA)
    },
    {
        sortPattern: {sortFieldA: 1, sortFieldB: 1},
        cmpFunction: (docA, docB) => parseInt(docA.sortFieldA) < parseInt(docB.sortFieldA) ||
            (parseInt(docA.sortFieldA) == parseInt(docB.sortFieldA) &&
             parseInt(docA.sortFieldB) < parseInt(docB.sortFieldB))
    },
    {
        sortPattern: {sortFieldA: -1, sortFieldB: -1},
        cmpFunction: (docA, docB) => parseInt(docA.sortFieldA) > parseInt(docB.sortFieldA) ||
            (parseInt(docA.sortFieldA) == parseInt(docB.sortFieldA) &&
             parseInt(docA.sortFieldB) > parseInt(docB.sortFieldB))
    },
];

// Cases where the children of the $or all require a FETCH.
(function testFetchedChildren() {
    const kFilterPredicates = [
        // $or with two children.
        {$or: [{filterFieldA: "4", filterFieldB: "4"}, {filterFieldA: "3", filterFieldB: "3"}]},

        // $or with three children.
        {
            $or: [
                {filterFieldA: "4", filterFieldB: "4"},
                {filterFieldA: "3", filterFieldB: "3"},
                {filterFieldA: "1", filterFieldB: "1"}
            ]
        },

        // $or with four children.
        {
            $or: [
                {filterFieldA: "4", filterFieldB: "4"},
                {filterFieldA: "3", filterFieldB: "3"},
                {filterFieldA: "2", filterFieldB: "2"},
                {filterFieldA: "1", filterFieldB: "1"}
            ]
        },
    ];

    runTest(kSorts, kFilterPredicates);
})();

// Cases where the children of the $or are IXSCANs.
(function testUnfetchedChildren() {
    const kFilterPredicates = [
        // $or with two children.
        {$or: [{sortFieldA: "4", sortFieldB: "4"}, {sortFieldA: "3", sortFieldB: "3"}]},

        // $or with three children.
        {
            $or: [
                {sortFieldA: "4", sortFieldB: "4"},
                {sortFieldA: "3", sortFieldB: "3"},
                {sortFieldA: "7", sortFieldB: "4"}
            ]
        },

        // $or with four children.
        {
            $or: [
                {sortFieldA: "4", sortFieldB: "4"},
                {sortFieldA: "3", sortFieldB: "3"},
                {sortFieldA: "7", sortFieldB: "4"},
                {sortFieldA: "10", sortFieldB: "9"}
            ]
        },
    ];

    runTest(kSorts, kFilterPredicates);
})();
})();
