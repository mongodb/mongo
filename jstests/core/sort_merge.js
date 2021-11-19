/**
 * Tests $or queries which can be answered with a SORT_MERGE stage.
 *
 * @tags: [
 *   assumes_read_concern_local,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/fixture_helpers.js");  // For 'isMongos'.

const coll = db.sort_merge;
coll.drop();

assert.commandWorked(coll.createIndex({filterFieldA: 1, sortFieldA: 1, sortFieldB: 1}));
assert.commandWorked(coll.createIndex({filterFieldA: 1, sortFieldA: -1, sortFieldB: -1}));
assert.commandWorked(coll.createIndex({sortFieldA: 1, sortFieldB: 1}));

// Insert some random data and dump the seed to the log.
const seed = new Date().getTime();
Random.srand(seed);
print("Seed for test is " + seed);

const kRandomValCeil = 5;
// Generates a random value between [0, kRandomValCeil).
function randomInt() {
    return Math.floor(Math.random() * kRandomValCeil);
}

for (let i = 0; i < 100; ++i) {
    assert.commandWorked(coll.insert({
        filterFieldA: randomInt(),
        filterFieldB: randomInt(),
        sortFieldA: randomInt(),
        sortFieldB: randomInt()
    }));
}

function isSorted(array, lessThanFunction) {
    for (let i = 1; i < array.length; ++i) {
        if (lessThanFunction(array[i], array[i - 1])) {
            return false;
        }
    }
    return true;
}

// Verifies that 'sortMerge' is covered, that is, it has no child FETCH or COLLSCAN stages.
function verifyCoveredPlan(explain, sortMerge) {
    assert(isIndexOnly(db, sortMerge), explain);
}

// Verifies that 'sortMerge' is not a covered plan. In particular, we check that the number of
// FETCH stages matches the number of branches and that each FETCH has an IXSCAN as a child.
function verifyNonCoveredPlan(explain, sortMerge) {
    assert(sortMerge.hasOwnProperty("inputStages"), explain);
    const numBranches = sortMerge.inputStages.length;
    const fetchStages = getPlanStages(sortMerge, "FETCH");

    assert.eq(fetchStages.length, numBranches, explain);
    for (const fetch of fetchStages) {
        assert(isIxscan(db, fetch), explain);
    }
}

// Verifies that 'sortMerge', which is a plan with a mix of non-covered and covered branches,
// has as many IXSCAN stages as there are branches.
function verifyMixedPlan(explain, sortMerge) {
    assert(sortMerge.hasOwnProperty("inputStages"), explain);
    const numBranches = sortMerge.inputStages.length;
    const ixscanStages = getPlanStages(sortMerge, "IXSCAN");

    // Note that this is not as strong an assertion as 'isIndexOnly' as some branches will have
    // FETCH stages.
    assert.eq(ixscanStages.length, numBranches, explain);
}

// Verifies that the query solution tree produced by 'sort' and 'filter' produces a SORT_MERGE
// plan and invokes 'callback' to make custom assertions about the SORT_MERGE plan.
function verifyPlan(sort, filter, callback) {
    const explain = coll.find(filter).sort(sort.sortPattern).explain("queryPlanner");

    // Search for all SORT_MERGE stages in case this is a sharded collection.
    if (FixtureHelpers.isMongos(db)) {
        const sortMergeStages = getPlanStages(explain, "SORT_MERGE");
        for (const sortMerge of sortMergeStages) {
            callback(explain, sortMerge);
        }
    } else {
        const sortMerge = getPlanStage(explain, "SORT_MERGE");
        callback(explain, sortMerge);
    }
}

function runTest(sorts, filters, verifyCallback) {
    for (let sortInfo of sorts) {
        for (let filter of filters) {
            verifyPlan(sortInfo, filter, verifyCallback);
            const res = coll.find(filter).sort(sortInfo.sortPattern).toArray();
            assert(isSorted(res, sortInfo.cmpFunction),
                   () => "Assertion failed for filter: " + tojson(filter) + "\n" +
                       "sort pattern " + tojson(sortInfo.sortPattern));

            // Check that there are no duplicates.
            let ids = new Set();
            for (let doc of res) {
                assert(!ids.has(doc._id), () => "Duplicate _id: " + tojson(_id));
                ids.add(doc._id);
            }
        }
    }
}

// Cases where the children of the $or all require a FETCH.
(function testFetchedChildren() {
    const kFilterPredicates = [
        // $or with two children.
        {$or: [{filterFieldA: 4, filterFieldB: 4}, {filterFieldA: 3, filterFieldB: 3}]},

        // $or with three children.
        {
            $or: [
                {filterFieldA: 4, filterFieldB: 4},
                {filterFieldA: 3, filterFieldB: 3},
                {filterFieldA: 1, filterFieldB: 1}
            ]
        },

        // $or with four children.
        {
            $or: [
                {filterFieldA: 4, filterFieldB: 4},
                {filterFieldA: 3, filterFieldB: 3},
                {filterFieldA: 2, filterFieldB: 2},
                {filterFieldA: 1, filterFieldB: 1}
            ]
        },
    ];

    const kSorts = [
        {
            sortPattern: {sortFieldA: 1},
            cmpFunction: (docA, docB) => docA.sortFieldA < docB.sortFieldA
        },
        {
            sortPattern: {sortFieldA: -1},
            cmpFunction: (docA, docB) => docA.sortFieldA > docB.sortFieldA
        },
        {
            sortPattern: {sortFieldA: 1, sortFieldB: 1},
            cmpFunction: (docA, docB) => docA.sortFieldA < docB.sortFieldA ||
                (docA.sortFieldA === docB.sortFieldA && docA.sortFieldB < docB.sortFieldB)
        },
        {
            sortPattern: {sortFieldA: -1, sortFieldB: -1},
            cmpFunction: (docA, docB) => docA.sortFieldA > docB.sortFieldA ||
                (docA.sortFieldA === docB.sortFieldA && docA.sortFieldB > docB.sortFieldB)
        },
    ];

    runTest(kSorts, kFilterPredicates, verifyNonCoveredPlan);
})();

// Cases where the children of the $or are NOT fetched.
(function testUnfetchedChildren() {
    const kFilterPredicates = [
        {
            $or: [
                {sortFieldA: 1, sortFieldB: 3},
                {filterFieldA: 2, sortFieldB: 2},
            ]
        },
        {
            $or: [
                {sortFieldA: 1, sortFieldB: 3},
                {sortFieldA: 2, sortFieldB: 2},
                {sortFieldA: 4, sortFieldB: 2},
            ]
        },
        {
            $or: [
                {sortFieldA: 1, sortFieldB: 3},
                {sortFieldA: 2, sortFieldB: 2},
                {sortFieldA: 4, sortFieldB: 2},
                {sortFieldA: 3, sortFieldB: 1},
            ]
        },
    ];

    const kSorts = [
        {
            sortPattern: {sortFieldB: 1},
            cmpFunction: (docA, docB) => docA.sortFieldB < docB.sortFieldB
        },
        {
            sortPattern: {sortFieldA: -1},
            cmpFunction: (docA, docB) => docA.sortFieldA > docB.sortFieldA
        },
        {
            sortPattern: {sortFieldA: 1, sortFieldB: 1},
            cmpFunction: (docA, docB) => docA.sortFieldA < docB.sortFieldA ||
                (docA.sortFieldA === docB.sortFieldA && docA.sortFieldB < docB.sortFieldB)
        },
        {
            sortPattern: {sortFieldB: 1, sortFieldA: 1},
            cmpFunction: (docA, docB) => docA.sortFieldB < docB.sortFieldB ||
                (docA.sortFieldB === docB.sortFieldB && docA.sortFieldA < docB.sortFieldA)
        },
        {
            sortPattern: {sortFieldA: 1, sortFieldB: -1},
            cmpFunction: (docA, docB) => docA.sortFieldA < docB.sortFieldA ||
                (docA.sortFieldA === docB.sortFieldA && docA.sortFieldB > docB.sortFieldB)
        },
    ];
    runTest(kSorts, kFilterPredicates, verifyCoveredPlan);
})();

// Cases where the children of the $or are a mix of fetched and unfetched.
(function testFetchedAndUnfetchedChildren() {
    const kFilterPredicates = [
        {$or: [{sortFieldA: 1, sortFieldB: 2}, {sortFieldA: 2, sortFieldB: 2, filterFieldB: 1}]},
        {
            $or: [
                {sortFieldA: 1, sortFieldB: 2},
                {sortFieldA: 2, sortFieldB: 2, filterFieldB: 1},
                {sortFieldA: 3, sortFieldB: 4, filterFieldA: 2},
                {sortFieldA: 4, sortFieldB: 1, filterFieldB: 4}
            ]
        },
    ];

    const kSorts = [
        {
            sortPattern: {sortFieldB: 1},
            cmpFunction: (docA, docB) => docA.sortFieldB < docB.sortFieldB
        },
        {
            sortPattern: {sortFieldA: -1},
            cmpFunction: (docA, docB) => docA.sortFieldA > docB.sortFieldA
        },
        {
            sortPattern: {sortFieldA: 1, sortFieldB: 1},
            cmpFunction: (docA, docB) => docA.sortFieldA < docB.sortFieldA ||
                (docA.sortFieldA === docB.sortFieldA && docA.sortFieldB < docB.sortFieldB)
        },
        {
            sortPattern: {sortFieldB: 1, sortFieldA: 1},
            cmpFunction: (docA, docB) => docA.sortFieldB < docB.sortFieldB ||
                (docA.sortFieldB === docB.sortFieldB && docA.sortFieldA < docB.sortFieldA)
        },
        {
            sortPattern: {sortFieldA: 1, sortFieldB: -1},
            cmpFunction: (docA, docB) => docA.sortFieldA < docB.sortFieldA ||
                (docA.sortFieldA === docB.sortFieldA && docA.sortFieldB > docB.sortFieldB)
        },
    ];
    runTest(kSorts, kFilterPredicates, verifyMixedPlan);
})();

// Insert documents with arrays into the collection and check that the deduping works correctly.
(function testDeduplication() {
    assert.commandWorked(coll.insert([
        {filterFieldA: [1, 2], filterFieldB: "multikeydoc", sortFieldA: 1, sortFieldB: 1},
        {sortFieldA: [1, 2], filterFieldA: "multikeydoc"}
    ]));

    const kUniqueFilters = [
        {
            $or: [
                {filterFieldA: 1, filterFieldB: "multikeydoc"},
                {filterFieldA: 2, filterFieldB: "multikeydoc"}
            ],
        },
        {
            $or: [
                {sortFieldA: 1, filterFieldA: "multikeydoc"},
                {sortFieldA: 2, filterFieldA: "multikeydoc"}
            ]
        }
    ];

    for (const filter of kUniqueFilters) {
        // Both branches of the $or will return the same document, which should be de-duped. Make
        // sure only one document is returned from the server.
        assert.eq(coll.find(filter).sort({sortFieldA: 1}).itcount(), 1);
    }
})();

// Verify that sort merge works correctly when sorting dotted paths.
(function testDottedPathSortMerge() {
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({'filterFieldA': 1, 'sortField.a': 1, 'sortField.b': 1}));
    for (let i = 0; i < 100; ++i) {
        assert.commandWorked(coll.insert({
            filterFieldA: randomInt(),
            filterFieldB: randomInt(),
            sortField: {a: randomInt(), b: randomInt()}
        }));
    }

    const kSortPattern = {
        sortPattern: {'sortField.a': 1, 'sortField.b': 1},
        cmpFunction: (docA, docB) => docA.sortField.a < docB.sortField.a ||
            (docA.sortField.a === docB.sortField.a && docA.sortField.b < docB.sortField.b)
    };

    const kNonCoveredFilter = {
        $or: [{filterFieldA: 1, filterFieldB: 2}, {filterFieldA: 2, filterFieldB: 1}]
    };
    runTest([kSortPattern], [kNonCoveredFilter], verifyNonCoveredPlan);

    const kCoveredFilter = {
        $or: [
            {filterFieldA: 1, 'sortField.a': 1},
            {filterFieldA: 2, 'sortField.a': 3},
            {filterFieldA: 3, 'sortField.a': 4, 'sortField.b': 3},
        ]
    };
    runTest([kSortPattern], [kCoveredFilter], verifyCoveredPlan);
})();
})();
