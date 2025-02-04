/**
 * An identity view has an empty pipeline but a different namespace. This test confirms that a
 * $lookup.$search query on an identity view returns correct results.
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {assertViewAppliedCorrectly} from "jstests/with_mongot/e2e/lib/explain_utils.js";

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

let viewName = "identityView";
assert.commandWorked(testDb.createView(viewName, foreignColl.getName(), []));
let identityView = testDb[viewName];

createSearchIndex(identityView,
                  {name: "identityViewIx", definition: {"mappings": {"dynamic": true}}});

let searchQuery = {
    $search: {
        index: "identityViewIx",
        exists: {
            path: "state",
        }
    }
};

let lookupPipeline = [{
    $lookup: {
        from: identityView.getName(),
        localField: "_id",
        foreignField: "state",
        pipeline: [
            searchQuery,
            {$sort : {city: 1}},
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
    {_id: "New Jersey", stateFacts: [{city: "Harrison", sportsTeam: "NJ/NY Gotham FC", pop: 5}]},
    {_id: "New York", stateFacts: [{city: "New York", sportsTeam: "NY Liberty", pop: 7}]}
];
let results = localColl.aggregate(lookupPipeline).toArray();
assert.eq(results, expectedResults);
dropSearchIndex(identityView, {name: "identityViewIx"});
