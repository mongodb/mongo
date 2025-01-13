/**
 * This test test considers $search queries with 'storedSource.' Specifically, it confirms that
 * Pipeline::makePipelineFromViewDefinition, which is responsible for constructing unionWith
 * subpipelines on views, doesn't apply the view stages to the top-level agg or to idLookup. This is
 * because mongot returns the full, transformed document for storedSource queries so mongod should
 * never apply the view.
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {
    assertViewNotApplied,
    extractUnionWithSubPipelineExplainOutput
} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const primaryStateFacts = testDb.primaryStateFacts;
primaryStateFacts.drop();

assert.commandWorked(primaryStateFacts.insertMany([
    {state: "NY", pop: 19000000, facts: {state_motto: "Excelsior", state_flower: "Rose"}},
    {
        state: "CA",
        pop: 39000000,
        facts: {
            state_motto: "Eureka",
            state_flower: "California Poppy",
            official_state_colors: ["blue", "gold"]
        }
    },
    {
        state: "NJ",
        pop: 9000000,
        facts: {
            state_motto: "Liberty and Prosperity",
            state_flower: "Common Blue Violet",
            official_state_colors: ["jersey blue", "bluff"]
        }
    },
    {
        state: "AR",
        pop: 3000000,
        facts: {state_motto: "Regnat Populus", state_flower: "Forget-Me-Not"}
    },
]));
let viewName = "officialStateColors";
let stateColorViewPipeline = [
    {"$addFields": {"facts.official_state_colors": {$ifNull: ["$facts.official_state_colors", []]}}}
];
assert.commandWorked(
    testDb.createView(viewName, primaryStateFacts.getName(), stateColorViewPipeline));
let officialStateColorsView = testDb[viewName];
let indexDef = {mappings: {dynamic: true}, storedSource: {exclude: ["facts.state_flower"]}};
createSearchIndex(officialStateColorsView, {name: "excludeStateFlower", definition: indexDef});

const secondaryStateFacts = testDb.secondaryStateFacts;
secondaryStateFacts.drop();

assert.commandWorked(secondaryStateFacts.insertMany([
    {
        state: "NY",
        governor: "Kathy Hochul",
        facts: {state_bird: "Eastern Bluebird", most_popular_sandwich: "Pastrami on Rye"}
    },
    {
        state: "CA",
        governor: "Gavin Newsom",
        facts: {state_bird: "California Quail", most_popular_sandwich: "Turkey on Dutch Crunch"}
    },
    {
        state: "NJ",
        governor: "Phil Murphy",
        facts: {state_bird: "Eastern Goldfinch", most_popular_sandwich: "Italian Hoagie"}
    },
    {
        state: "AR",
        governor: "Sarah Huckabee Sanders",
        facts: {state_bird: "Northern Mockingbird", most_popular_sandwich: "Fried Bologna"}
    },
    {
        state: "Hawaii",
        governor: "Josh Green",
        facts: {state_bird: "Nene", most_popular_sandwich: "Kalua Pork"}
    }
]));

viewName = "topHalloweenCandyByState";
let halloweenCandyViewPipeline = [{
    $addFields: {
        "facts.top_halloween_candy": {
            $cond: [

                {
                    $not: {
                        $in: [
                            "$state",
                            [
                                "Idao",
                                "North dakota",
                                "Nebraska",
                                "South Dakota",
                                "Utah",
                                "Montana",
                                "Hawaii"
                            ]
                        ]
                    }
                },
                "reese's peanut butter cups",  // Any state not in the array will be assigned this.
                "m&ms"                         // Idaho, Hawaii, etc will be assigned m&ms.
            ]
        }
    }
}];
assert.commandWorked(
    testDb.createView(viewName, secondaryStateFacts.getName(), halloweenCandyViewPipeline));
let halloweenCandyView = testDb[viewName];
indexDef = {
    mappings: {dynamic: true},
    storedSource: {exclude: ["facts.state_bird"]}
};
createSearchIndex(halloweenCandyView, {name: "excludeStateBird", definition: indexDef});

/**
 * This query runs the outer $search query on a view and the inner unionWith.$search subpipeline on
 * a view.
 */
let pipeline = [
    {
        $search: {
            index: "excludeStateFlower",
            wildcard: {
                query: "*",  // This matches all documents
                path: "state",
                allowAnalyzedField: true,
            },
            returnStoredSource: true
        },
    },
    {$set: {source: officialStateColorsView.getName()}},
    {$project: {_id: 0}},
    {
        $unionWith: {
            coll: halloweenCandyView.getName(),
            pipeline: [
                {
                    $search: {
                        index: "excludeStateBird",
                        wildcard: {
                            query: "*",  // This matches all documents
                            path: "state",
                            allowAnalyzedField: true,
                        },
                        returnStoredSource: true
                    }
                },
                {$set: {source: halloweenCandyView.getName()}},
                {$project: {_id: 0}},
            ]
        }
    }
];

let explain = officialStateColorsView.explain().aggregate(pipeline).stages;
// Confirm top-level aggregation doesn't include view stages as it's a stored source query, mongot
// should be returning the transformed fields.
assertViewNotApplied(explain, stateColorViewPipeline);
// Confirm unionWith subpipeline doesn't include view stages as it's also a stored source query.
let unionWithSubPipeExplain = extractUnionWithSubPipelineExplainOutput(explain);
assertViewNotApplied(unionWithSubPipeExplain, halloweenCandyViewPipeline);

/**
 * The results should not include facts.state_flower or facts.state_bird but should include
 * facts.official_state_colors and facts.top_halloween_candy.
 */
let expectedResults = [
    {
        state: "CA",
        pop: 39000000,
        facts: {state_motto: "Eureka", official_state_colors: ["blue", "gold"]},
        source: "officialStateColors"
    },
    {
        state: "NY",
        pop: 19000000,
        facts: {state_motto: "Excelsior", official_state_colors: []},
        source: "officialStateColors"
    },
    {
        state: "AR",
        pop: 3000000,
        facts: {state_motto: "Regnat Populus", official_state_colors: []},
        source: "officialStateColors"
    },
    {
        state: "NJ",
        pop: 9000000,
        facts: {
            state_motto: "Liberty and Prosperity",
            official_state_colors: ["jersey blue", "bluff"]
        },
        source: "officialStateColors"
    },
    {
        state: "CA",
        governor: "Gavin Newsom",
        facts: {
            most_popular_sandwich: "Turkey on Dutch Crunch",
            top_halloween_candy: "reese's peanut butter cups"
        },
        source: "topHalloweenCandyByState"
    },
    {
        state: "NJ",
        governor: "Phil Murphy",
        facts: {
            most_popular_sandwich: "Italian Hoagie",
            top_halloween_candy: "reese's peanut butter cups"
        },
        source: "topHalloweenCandyByState"
    },
    {
        state: "Hawaii",
        governor: "Josh Green",
        facts: {most_popular_sandwich: "Kalua Pork", top_halloween_candy: "m&ms"},
        source: "topHalloweenCandyByState"
    },
    {
        state: "NY",
        governor: "Kathy Hochul",
        facts: {
            most_popular_sandwich: "Pastrami on Rye",
            top_halloween_candy: "reese's peanut butter cups"
        },
        source: "topHalloweenCandyByState"
    },
    {
        state: "AR",
        governor: "Sarah Huckabee Sanders",
        facts: {
            most_popular_sandwich: "Fried Bologna",
            top_halloween_candy: "reese's peanut butter cups"
        },
        source: "topHalloweenCandyByState"
    }
];

let results = officialStateColorsView.aggregate(pipeline).toArray();
assertArrayEq({actual: results, expected: expectedResults});

dropSearchIndex(officialStateColorsView, {name: "excludeStateFlower"});
dropSearchIndex(halloweenCandyView, {name: "excludeStateBird"});
