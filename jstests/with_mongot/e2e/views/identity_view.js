/**
 * An identity view has an empty pipeline but a different namespace. This test confirms that a
 * $lookup.$search query on an identity view returns correct results.
 *
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    assertLookupInExplain,
} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {
    createSearchIndexesAndExecuteTests,
    validateSearchExplain
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const localColl = testDb.localColl;
localColl.drop();
assert.commandWorked(localColl.insertMany([
    {_id: "New York"},
    {_id: "Kansas"},
    {_id: "New Jersey"},
    {_id: "California"},
    {_id: "Missouri"}
]));

const foreignColl = testDb.underlyingSourceCollection;
foreignColl.drop();
assert.commandWorked(foreignColl.insertMany([
    {city: "New York", state: "New York", sportsTeam: "NY Liberty", pop: 7},
    {city: "Oakland", state: "California", sportsTeam: "Golden State Valkyries", pop: 6},
    {
        city: "Berkeley",
        state: "California",
    },
    {
        city: "Kansas City",
        state: "Kansas",
        sportsTeam: "KC Current",
    },
    {
        city: "St Louis",
        state: "Missouri",
        sportsTeam: "St Louis Slam",
    },
    {city: "Richmond", state: "California", pop: 4},
    {city: "Harrison", state: "New Jersey", sportsTeam: "NJ/NY Gotham FC", pop: 5}
]));

const viewName = "identityView";
assert.commandWorked(testDb.createView(viewName, foreignColl.getName(), []));
const identityView = testDb[viewName];

const indexConfig = {
    coll: identityView,
    definition: {name: "identityViewIx", definition: {"mappings": {"dynamic": true}}}
};

const identityViewTestCases = (isStoredSource) => {
    // ===================================================================================
    // Case 1: Identity view with $lookup.$search.
    // ===================================================================================
    const searchQuery = {
        index: "identityViewIx",
        exists: {
            path: "state",
        },
        returnStoredSource: isStoredSource
    };

    const lookupPipeline = [{
            $lookup: {
                from: identityView.getName(),
                localField: "_id",
                foreignField: "state",
                pipeline: [
                    {$search: searchQuery},
                    {$sort: {city: 1}},
                    {$project: {_id: 0}}],
                    as: "stateFacts"
                }
            },
            {$sort: {_id: 1}},
            {$project: {"stateFacts.state": 0}}
        ];

    let expectedResults = [
        {
            _id: "California",
            stateFacts: [
                {city: "Berkeley"},
                {city: "Oakland", sportsTeam: "Golden State Valkyries", pop: 6},
                {city: "Richmond", pop: 4}
            ]
        },
        {_id: "Kansas", stateFacts: [{city: "Kansas City", sportsTeam: "KC Current"}]},
        {_id: "Missouri", stateFacts: [{city: "St Louis", sportsTeam: "St Louis Slam"}]},
        {
            _id: "New Jersey",
            stateFacts: [{city: "Harrison", sportsTeam: "NJ/NY Gotham FC", pop: 5}]
        },
        {_id: "New York", stateFacts: [{city: "New York", sportsTeam: "NY Liberty", pop: 7}]}
    ];

    validateSearchExplain(localColl, lookupPipeline, isStoredSource, null, (explain) => {
        assertLookupInExplain(explain, lookupPipeline[0]);
    });

    let results = localColl.aggregate(lookupPipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});

    // ===================================================================================
    // Case 2: Identity view with $unionWith.$search.
    // ===================================================================================
    const unionWithPipeline = [
        {$sort: {_id: 1}},
        {$limit: 1},
        {
            $unionWith: {
                coll: identityView.getName(),
                pipeline: [
                    {$search: searchQuery},
                    {$sort: {city: 1}},
                    {$project: {_id: 0, "stateFacts.state": 0}},
                    {$limit: 1}
                ]
            }
        }
    ];

    expectedResults = [
        {_id: "California"},
        {
            city: "Berkeley",
            state: "California",
        },
    ];

    validateSearchExplain(localColl, unionWithPipeline, isStoredSource);

    results = localColl.aggregate(unionWithPipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});
};

createSearchIndexesAndExecuteTests(indexConfig, identityViewTestCases);
