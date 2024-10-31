/**
 * $lookup filters the foreign collection via an equality match eg:
 * { <foreignFieldName> : { "$eq" : <localFieldList[0]> } }
 *
 * When the foreign collection is a view, the view transforms must be applied before the $match.
 * However for mongot queries, _idLookup applies the view transforms. In this way, the $match needs
 * to be applied directly after the $search/$searchMeta stage and before the other stages in the
 * user pipeline.
 *
 * This test ensures the ordering of stages for a $lookup.$search query, where the foreign coll is a
 * view, is correct.
 *
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {assertViewAppliedCorrectly} from "jstests/with_mongot/e2e/lib/explain_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const localColl = testDb.localColl;
localColl.drop();
assert.commandWorked(localColl.insertMany([{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]));

const foreignColl = testDb.underlyingSourceCollection;
foreignColl.drop();
assert.commandWorked(foreignColl.insertMany([
    {x: 2, debugMsg: "should match _id 2 in localColl", foobarRepeatedXTimes: "foobarfoobar"},
    {x: 3, debugMsg: "should match _id 3 in localColl", foobarRepeatedXTimes: null},
    {x: 4, debugMsg: "should match _id 4 in localColl", foobarRepeatedXTimes: null},
    {x: 5, debugMsg: "should match _id 5 in localColl", foobarRepeatedXTimes: null},
]));

let viewName = "addFields";
/**
 * This view enriches the foobarRepeatedXTimes field. If foobarRepeatedXTimes is null, the view
 * concatenates the string foobar x (the field in the doc) number of times.
 */
let viewPipeline = [{
    "$addFields": {
        "foobarRepeatedXTimes": {
            "$cond": {
                "if": {"$eq": ["$foobarRepeatedXTimes", null]},
                "then": {
                    "$reduce": {
                        "input": {"$range": [0, "$x"]},
                        "initialValue": "",
                        "in": {"$concat": ["$$value", "foobar"]}
                    }
                },
                "else": "$foobarRepeatedXTimes"
            }
        }
    }
}];
assert.commandWorked(testDb.createView(viewName, 'underlyingSourceCollection', viewPipeline));
let addFieldsView = testDb[viewName];

createSearchIndex(addFieldsView,
                  {name: "populationAddFieldsIndex", definition: {"mappings": {"dynamic": true}}});

let searchQuery = {
    $search: {
        index: "populationAddFieldsIndex",
        exists: {
            path: "foobarRepeatedXTimes",
        }
    }
};

/**
 * This query has a subpipeline that sets x to x+1 for the documents in the
 * foreign collection. This is to ensures that the equality match between "_id" in localColl and "x"
 * in foreignColl happens after $search and but before the rest of the user pipeline (eg before that
 * $set stage). In other words, the check happens at _id === x and not _id === (x + 1)
 */
let lookupPipeline = 
[
    {
        $lookup: {
            from: addFieldsView.getName(),
            localField: "_id",
            foreignField: "x",
            pipeline: [
                searchQuery,
                {$project: {_id: 0}},
                {$set: {x: {$add: [1, "$x"]}}},
                ],
            as: "anything"
        },
    }, 
    {$sort: {_id: 1}}
];

let expectedResults = [
    {_id: 1, anything: []},
    {
        _id: 2,
        anything: [{
            x: 3,
            debugMsg: "should match _id 2 in localColl",
            foobarRepeatedXTimes: "foobarfoobar"
        }]
    },
    {
        _id: 3,
        anything: [{
            x: 4,
            debugMsg: "should match _id 3 in localColl",
            foobarRepeatedXTimes: "foobarfoobarfoobar"
        }]
    },
    {
        _id: 4,
        anything: [{
            x: 5,
            debugMsg: "should match _id 4 in localColl",
            foobarRepeatedXTimes: "foobarfoobarfoobarfoobar"
        }]
    }
];
let results = localColl.aggregate(lookupPipeline).toArray();
assertArrayEq({actual: results, expected: expectedResults});
dropSearchIndex(addFieldsView, {name: "populationAddFieldsIndex"});
