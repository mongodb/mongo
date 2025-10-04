/**
 * This test checks the lookup cache codepath for $search subpipelines on
 * views.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {assertLookupInExplain} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {
    createSearchIndexesAndExecuteTests,
    validateSearchExplain,
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const localColl = testDb.localColl;
localColl.drop();
assert.commandWorked(
    localColl.insertMany([
        {
            "_id": 1,
            "student": "Ann Aardvark",
            sickdays: [new Date("2024-05-01"), new Date("2024-08-23")],
        },
        {"_id": 5, "student": "Zoe Zebra", sickdays: [new Date("2024-02-01"), new Date("2024-05-23")]},
    ]),
);

const holidaysColl = testDb.holidays;
holidaysColl.drop();
assert.commandWorked(
    holidaysColl.insertMany([
        {
            _id: 1,
            year: 2024,
            name: "National Inane Answering Message Day",
            date: new Date("2024-01-30"),
        },
        {_id: 2, year: 2024, name: "National Have A Bad Day Day", date: new Date("2024-11-19")},
        {_id: 3, year: 2024, name: "National Sock Monkey Day", date: new Date("2024-03-07")},
        {
            _id: 4,
            year: 2024,
            name: "What If Cats and Dogs Had Opposable Thumbs? Day",
            date: new Date("2024-01-01"),
        },
        {_id: 5, year: 2022, name: "National Do a Grouch a Favor Day", date: new Date("2022-02-16")},
    ]),
);

// Filter for holidays in Jan, Feb or March.
const viewPipeline = [{"$match": {"$expr": {"$in": [{"$month": "$date"}, [1, 2, 3]]}}}];
const viewName = "firstThreeMonths";
assert.commandWorked(testDb.createView(viewName, holidaysColl.getName(), viewPipeline));
const firstThreeMonthsView = testDb[viewName];

const indexConfig = {
    coll: firstThreeMonthsView,
    definition: {name: "sillyHolidaysInFirstThreeMonthsIx", definition: {"mappings": {"dynamic": true}}},
};

const lookupCacheTestCases = (isStoredSource) => {
    const searchQuery = {
        $search: {
            index: "sillyHolidaysInFirstThreeMonthsIx",
            text: {query: "National", path: "name"},
            returnStoredSource: isStoredSource,
        },
    };

    // ===================================================================================
    // Case 1: Basic $lookup.$search with cache
    // ===================================================================================

    /*
     * When local/foreignFields are absent in the $lookup query, the cache is enabled. Moreover,
     * the subpipeline doesn't reference the let variables so the result should be cached (and
     * the same) every time.
     */
    let lookupPipeline = [
        {
            $lookup: {
                from: firstThreeMonthsView.getName(),
                pipeline: [
                    searchQuery,
                    {$match: {year: 2024}},
                    {$project: {_id: 0, date: {name: "$name", date: "$date"}}},
                    {$replaceRoot: {newRoot: "$date"}},
                    {$sort: {date: -1}},
                ],
                as: "holidays",
            },
        },
    ];

    let expectedResults = [
        {
            _id: 1,
            student: "Ann Aardvark",
            sickdays: [ISODate("2024-05-01T00:00:00Z"), ISODate("2024-08-23T00:00:00Z")],
            holidays: [
                {name: "National Sock Monkey Day", date: ISODate("2024-03-07T00:00:00Z")},
                {
                    name: "National Inane Answering Message Day",
                    date: ISODate("2024-01-30T00:00:00Z"),
                },
            ],
        },
        {
            _id: 5,
            student: "Zoe Zebra",
            sickdays: [ISODate("2024-02-01T00:00:00Z"), ISODate("2024-05-23T00:00:00Z")],
            holidays: [
                {name: "National Sock Monkey Day", date: ISODate("2024-03-07T00:00:00Z")},
                {
                    name: "National Inane Answering Message Day",
                    date: ISODate("2024-01-30T00:00:00Z"),
                },
            ],
        },
    ];

    validateSearchExplain(localColl, lookupPipeline, isStoredSource, null, (explain) => {
        assertLookupInExplain(explain, lookupPipeline[0]);
    });

    let results = localColl.aggregate(lookupPipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    // ===================================================================================
    // Case 2: Suffix added to the subpipeline.
    // ===================================================================================

    // We've added a suffix to the subpipeline which does reference the let variable (studentID).
    // The prefix should still be cached while the suffix should be re-run every time by unspooling
    // the cache (accessing the data in the cache).
    lookupPipeline = [
        {
            $lookup: {
                from: firstThreeMonthsView.getName(),
                let: {studentID: "$_id"},
                pipeline: [
                    searchQuery,
                    {$match: {year: 2024}},
                    {$project: {_id: 0, date: {name: "$name", date: "$date"}}},
                    {$replaceRoot: {newRoot: "$date"}},
                    {$match: {$expr: {$eq: ["$$studentID", 1]}}}, // This will only return holiday entries for the student with _id 1.
                    {$sort: {date: -1}},
                ],
                as: "holidays",
            },
        },
    ];

    expectedResults = [
        {
            _id: 1,
            student: "Ann Aardvark",
            sickdays: [ISODate("2024-05-01T00:00:00Z"), ISODate("2024-08-23T00:00:00Z")],
            holidays: [
                {name: "National Sock Monkey Day", date: ISODate("2024-03-07T00:00:00Z")},
                {
                    name: "National Inane Answering Message Day",
                    date: ISODate("2024-01-30T00:00:00Z"),
                },
            ],
        },
        {
            _id: 5,
            student: "Zoe Zebra",
            sickdays: [ISODate("2024-02-01T00:00:00Z"), ISODate("2024-05-23T00:00:00Z")],
            holidays: [],
        },
    ];

    validateSearchExplain(localColl, lookupPipeline, isStoredSource, null, (explain) => {
        assertLookupInExplain(explain, lookupPipeline[0]);
    });

    results = localColl.aggregate(lookupPipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});
};

createSearchIndexesAndExecuteTests(indexConfig, lookupCacheTestCases);
