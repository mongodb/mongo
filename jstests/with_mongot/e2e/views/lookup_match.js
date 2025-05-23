/**
 * $lookup filters the foreign collection via an equality match eg:
 * { <foreignFieldName> : { "$eq" : <localFieldList[0]> } }
 *
 * When the foreign collection is a view, the view transforms must be applied before the $match.
 * However for mongot queries, _idLookup applies the view transforms. In this way, the $match needs
 * to be applied directly after the $search/$searchMeta stage and before the other stages in the
 * user pipeline.
 *
 * This test ensures the ordering of stages is correct for a $lookup.$search query where the foreign
 * coll is a view.
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
assert.commandWorked(localColl.insertMany([{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]));

const foreignColl = testDb.underlyingSourceCollection;
foreignColl.drop();
assert.commandWorked(foreignColl.insertMany([
    {x: 2, debugMsg: "should match _id 2 in localColl", foobarRepeatedXTimes: "foobarfoobar"},
    {x: 3, debugMsg: "should match _id 3 in localColl", foobarRepeatedXTimes: null},
    {x: 4, debugMsg: "should match _id 4 in localColl", foobarRepeatedXTimes: null},
    {x: 5, debugMsg: "should match _id 5 in localColl", foobarRepeatedXTimes: null},
]));

const viewName = "addFields";

// This view enriches the foobarRepeatedXTimes field. If foobarRepeatedXTimes is null, the view
// concatenates the string foobar x (the field in the doc) number of times.
const viewPipeline = [{
        $addFields: {
            foobarRepeatedXTimes: {
                $cond: {
                    if: {$eq: ["$foobarRepeatedXTimes", null]},
                    then: {
                        $reduce: {
                            input: {$range: [0, "$x"]},
                            initialValue: "",
                            in: {$concat: ["$$value", "foobar"]}
                        }
                    },
                    else: "$foobarRepeatedXTimes"
                }
            }
        }
    }];
assert.commandWorked(testDb.createView(viewName, 'underlyingSourceCollection', viewPipeline));
const addFieldsView = testDb[viewName];

const indexConfig = {
    coll: addFieldsView,
    definition: {name: "populationAddFieldsIndex", definition: {"mappings": {"dynamic": true}}}
};

const lookupMatchTestCases = (isStoredSource) => {
    // This query has a subpipeline that sets x = x + 1 for the documents in the foreign collection.
    // This is to ensure that the equality match between "_id" in localColl and "x" in foreignColl
    // happens after $search and but before the rest of the user pipeline (e.g before that $set
    // stage). In other words, the check happens at _id === x and not _id === (x + 1).
    const lookupPipeline = 
            [
                {
                    $lookup: {
                        from: addFieldsView.getName(),
                        localField: "_id",
                        foreignField: "x",
                        pipeline: [
                            {
                                $search: {
                                    index: "populationAddFieldsIndex",
                                    exists: {
                                        path: "foobarRepeatedXTimes",
                                    },
                                    returnStoredSource: isStoredSource
                                }
                            },
                            {$project: {_id: 0}},
                            {$set: {x: {$add: [1, "$x"]}}},
                        ],
                        as: "anything"
                    },
                }, 
                {$sort: {_id: 1}}
            ];

    const expectedResults = [
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

    validateSearchExplain(localColl, lookupPipeline, isStoredSource, null, (explain) => {
        assertLookupInExplain(explain, lookupPipeline[0]);
    });

    const results = localColl.aggregate(lookupPipeline).toArray();
    assertArrayEq({actual: results, expected: expectedResults});
};

createSearchIndexesAndExecuteTests(indexConfig, lookupMatchTestCases);
