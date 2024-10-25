/**
 * $searchMeta queries should not invoke view pipelines. The reasoning being that $searchMeta
 * results provide meta data on the enriched collection, they don't have/display the enriched fields
 * themselves. On an implementation level, $searchMeta doesn't desugar to $_internalSearchIdLookup
 * (which performs view transforms for other mongot operators).
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {assertViewAppliedCorrectly} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.hotelAccounting;
coll.drop();

assert.commandWorked(coll.insertMany([
    {_id: 0, room: "ocean view", roomFee: 450, cleaningFee: 100},      // 550
    {_id: 1, room: "city view", roomFee: 389, cleaningFee: 80},        // 469
    {_id: 2, room: "obstructed view", roomFee: 299, cleaningFee: 80},  // 379
    {_id: 3, room: "corner", roomFee: 421, cleaningFee: 94},           // 515
    {_id: 4, room: "twin room", roomFee: 309, cleaningFee: 66},        // 375
]));

let viewName = "totalPrice";
let viewPipeline = [{"$addFields": {totalPrice: {$add: ['$cleaningFee', "$roomFee"]}}}];
assert.commandWorked(testDb.createView(viewName, 'hotelAccounting', viewPipeline));
let totalPriceView = testDb[viewName];

createSearchIndex(totalPriceView, {
    name: "totalPriceIndex",
    definition: {"mappings": {"dynamic": false, "fields": {"totalPrice": {"type": "numberFacet"}}}}
});
// This query creates three buckets for totalPrice field: [300-399], [400-499], [500-599]
const facetQuery = [{
    $searchMeta: {
        index: "totalPriceIndex",
        facet: {
            facets: {
                "priceRanges":
                    {"type": "number", "path": "totalPrice", "boundaries": [300, 400, 500, 600]}
            },
        },
    }
}];

// Verify that the explain output doesn't contain the view pipeline.
let explain = totalPriceView.explain().aggregate(facetQuery);
assert(explain.stages.length == 1);
assert(Object.keys(explain.stages[0])[0], "$searchMeta");

let expectedResults = [{
    count: {lowerBound: NumberLong(5)},
    facet: {
        priceRanges: {
            buckets: [
                {_id: 300, count: NumberLong(2)},
                {_id: 400, count: NumberLong(1)},
                {_id: 500, count: NumberLong(2)}
            ]
        }
    }
}];

let results = totalPriceView.aggregate(facetQuery).toArray();
assert.eq(results, expectedResults);

// TODO SERVER-93638: uncomment this test when lookup.search is supported on views.
// // $lookup.$searchMeta for good measure!
// const collBase = testDb.base;
// collBase.drop();
// assert.commandWorked(collBase.insert({_id: 0}));
// assert.commandWorked(collBase.insert({_id: 1}));

// /**
//  * $lookup doesn't include any details about its subpipeline in the explain output. Therefore
//  there
//  * isn't any chance that the $lookup subpipeline will include the view pipeline in the explain
//  * output. Instead, we just verify that the top-level agg doesn't contain the view transforms.
//  */
// explain = collBase.explain().aggregate(
//     [{$lookup: {from: "totalPrice", pipeline: facetQuery, as: "meta_facet"}}]);
// assert(explain.stages.length == 1);
// assert(Object.keys(explain.stages[0])[0], "$lookup");

// expectedResults = [
//     {
//         _id: 0,
//         meta_facet: [{
//             count: {lowerBound: NumberLong(5)},
//             facet: {
//                 priceRanges: {
//                     buckets: [
//                         {_id: 300, count: NumberLong(2)},
//                         {_id: 400, count: NumberLong(1)},
//                         {_id: 500, count: NumberLong(2)}
//                     ]
//                 }
//             }
//         }]
//     },
//     {
//         _id: 1,
//         meta_facet: [{
//             count: {lowerBound: NumberLong(5)},
//             facet: {
//                 priceRanges: {
//                     buckets: [
//                         {_id: 300, count: NumberLong(2)},
//                         {_id: 400, count: NumberLong(1)},
//                         {_id: 500, count: NumberLong(2)}
//                     ]
//                 }
//             }
//         }]
//     }
// ];

// results =
//     collBase.aggregate([{$lookup: {from: "totalPrice", pipeline: facetQuery, as: "meta_facet"}}])
//         .toArray();
// assert.eq(expectedResults, results);

dropSearchIndex(totalPriceView, {name: "totalPriceIndex"});