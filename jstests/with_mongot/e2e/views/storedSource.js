/**
 * This test asserts that storedSource queries work correctly on views by inspecting explain()
 * output and document results.
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {assertViewNotApplied} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
coll.drop();

assert.commandWorked(coll.insertMany([
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
        state: "AK",
        pop: 3000000,
        facts: {state_motto: "Regnat Populus", state_flower: "Forget-Me-Not"}
    },
]));

let viewName = "addFields";
let viewPipeline = [
    {"$addFields": {"facts.official_state_colors": {$ifNull: ["$facts.official_state_colors", []]}}}
];
assert.commandWorked(testDb.createView(viewName, 'underlyingSourceCollection', viewPipeline));
let addFieldsView = testDb[viewName];

let indexDef = {mappings: {dynamic: true}, storedSource: {exclude: ["facts.state_flower"]}};

createSearchIndex(addFieldsView, {name: "storedSourceIx", definition: indexDef});

/**
 * Ensure the follow returnStoredSource query doesn't include view stages in the explain output or
 * facts.state_flower in the document results.
 */
let pipeline = [
    {
        $search: {
            index: "storedSourceIx",
            wildcard: {
                query: "*",  // This matches all documents
                path: "state",
                allowAnalyzedField: true,
            },
            returnStoredSource: true
        },
    },
    {$project: {_id: 0}}
];

let explain = addFieldsView.explain().aggregate(pipeline);
assertViewNotApplied(explain.stages, viewPipeline);

let expectedResults = [
    {
        state: "CA",
        pop: 39000000,
        facts: {state_motto: "Eureka", official_state_colors: ["blue", "gold"]},
    },
    {
        state: "AK",
        pop: 3000000,
        facts: {state_motto: "Regnat Populus", official_state_colors: []},
    },
    {
        state: "NY",
        pop: 19000000,
        facts: {state_motto: "Excelsior", official_state_colors: []},
    },
    {
        state: "NJ",
        pop: 9000000,
        facts: {
            state_motto: "Liberty and Prosperity",
            official_state_colors: ["jersey blue", "bluff"]
        },
    }
];
let results = addFieldsView.aggregate(pipeline).toArray();
assertArrayEq({actual: results, expected: expectedResults});
// TODO SERVER-93638 uncomment rest of test when $lookup.search on views support is enabled.
// // Make sure if storedSource query is part of a inner subpipeline, the view transforms aren't
// // applied by mongod.
// const baseColl = testDb.baseColl;
// baseColl.drop();
// assert.commandWorked(
//     baseColl.insertMany([{state: "AK"}, {state: "CA"}, {state: "NY"}, {state: "NJ"}]));

// let storedSourceAsSubPipe = [
//     {
//         $lookup: {
//             from: viewName,
//             localField: "state",
//             foreignField: "state",
//             as: "state_facts",
//             pipeline
//         }
//     }, {$project: {_id: 0, "state_facts.state": 0}}
// ];
// explain = baseColl.explain().aggregate(storedSourceAsSubPipe);
// assertViewNotApplied(explain.stages, viewPipeline);

// expectedResults = [
//     {
//         state: "AK",
//         state_facts:
//             [{pop: 3000000, facts: {state_motto: "Regnat Populus", official_state_colors: []}}]
//     },
//     {
//         state: "CA",
//         state_facts: [
//             {pop: 39000000,
//              facts: {state_motto: "Eureka", official_state_colors: ["blue", "gold"]}}
//         ]
//     },
//     {
//         state: "NY",
//         state_facts: [{pop: 19000000, facts: {state_motto: "Excelsior", official_state_colors:
//         []}}]
//     },
//     {
//         state: "NJ",
//         state_facts: [{
//             pop: 9000000,
//             facts: {
//                 state_motto: "Liberty and Prosperity",
//                 official_state_colors: ["jersey blue", "bluff"]
//             }
//         }]
//     }
// ];
// results = baseColl.aggregate(storedSourceAsSubPipe).toArray();
// assertArrayEq({actual: results, expected: expectedResults});

dropSearchIndex(addFieldsView, {name: "storedSourceIx"});