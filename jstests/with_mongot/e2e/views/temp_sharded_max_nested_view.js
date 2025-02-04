/**
 * This test nests views to the maximum amount allowed by the server, creates a search index on the
 * top view, and validates the execution of $search queries on the top view. Its intention is to
 * ensure that the correct `effectivePipeline` is passed to mongot upon creation of the search
 * index.
 *
 * This test does not use the search library functions as those
 * run $listSearchIndexes, which will not be supported until SERVER-99786.
 *
 * TODO SERVER-99786 : remove this test and just use max_nested_view.js once $listSearchIndexes can
 * support sharded views
 * @tags: [ featureFlagMongotIndexedViews, requires_fcv_81 ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
assertDropCollection(testDb, coll.getName());

let bulk = coll.initializeUnorderedBulkOp();
bulk.insert({_id: "New York", state: "NY", tags: ["east", "finance"], category: "large"});
bulk.insert({_id: "Oakland", state: "CA", tags: ["west", "bay"], category: "medium"});
bulk.insert({_id: "Palo Alto", state: "CA", tags: ["west", "tech", "bay"], category: "medium"});
bulk.insert({_id: "San Francisco", state: "CA", tags: ["west", "tech", "bay"], category: "large"});
bulk.insert({_id: "Trenton", state: "NJ", pop: 5, tags: ["east"], category: "small"});
assert.commandWorked(bulk.execute());

let parentName = "underlyingSourceCollection";

let officePotentialPipeline = [{
    "$addFields": {
        // Add a new field office_potential.
        office_potential: {
            "$switch": {
                branches: [
                    {
                        // High potential if the document is tagged as tech and is large.
                        case: {
                            "$and": [
                                {"$in": ["tech", "$nested_tags"]},
                                {"$eq": ["$nested_category", "large_level_17"]}
                            ]
                        },
                        then: "high"
                    },
                    {
                        // Medium potential if the document is tagged as tech and is medium.
                        case: {
                            "$and": [
                                {"$in": ["tech", "$nested_tags"]},
                                {"$eq": ["$nested_category", "medium_level_17"]}
                            ]
                        },
                        then: "medium"
                    },
                    // Medium potential if the document is large (and none of the above).
                    {case: {"$eq": ["$nested_category", "large_level_17"]}, then: "emerging"},
                ],
                // Low potential otherwise.
                default: "low"
            }
        },
    }
}];

// Create a max depth view (19 nested views + 1 source collection).
for (let i = 0; i < 19; ++i) {
    let childName = `nestedView${i}`;
    // Transformation summary:
    //  1. Adds a new numeric field "transformation_{i}": i
    //  2. Adds a new field "nested_tags" which appends a value "level_{i}" to the existing tags
    //  array. (e.g.  ['existing_tag', 'level_0', "level_1", etc.])
    //  3. Adds a new field "nested_category" which appends "_level_{i}" to the existing category
    //  value. (e.g. 'original_category_level_0', 'original_category_level_1', etc.)
    //  4. Adds a new field "dependent_transformation" which adds the current view i to the previous
    //  view i - 1 (e.g. nestedView10's value is 10 + 9 = 19). This is to ensure that each view can
    //  retrieve values from the view it depends on.
    let viewPipeline = [{
        "$addFields": {
            [`transformation_${i}`]: i,
            nested_tags: {$concatArrays: ["$tags", [`level_${i}`]]},
            nested_category: {$concat: ["$category", `_level_${i}`]},
            dependent_transformation:
                {$add: [{$ifNull: [`$transformation_${i > 0 ? i - 1 : 0}`, 0]}, i]}
        }
    }];

    assert.commandWorked(testDb.createView(childName, parentName, viewPipeline));

    // On the final iteration, create a separate view for office potential.
    if (i == 18) {
        assert.commandWorked(
            testDb.createView("officePotentialView", parentName, officePotentialPipeline));
    }

    parentName = childName;
}

// Get the deepest view.
let maxNestedView = testDb[parentName];
let officePotentialView = testDb["officePotentialView"];
let searchIndexName = "maxNestedViewIndex";

let searchIndexCommandResult = assert.commandWorked(testDb.runCommand({
    'createSearchIndexes': maxNestedView.getName(),
    'indexes': [{'name': searchIndexName, 'definition': {'mappings': {'dynamic': true}}}]
}));
assert.eq(searchIndexCommandResult.indexesCreated.length, 1);

searchIndexCommandResult = assert.commandWorked(
    testDb.runCommand({'dropSearchIndex': maxNestedView.getName(), name: searchIndexName}));
