/**
 * This test nests views to the maximum amount allowed by the server, creates a search index on the
 * top view, and validates the execution of $search queries on the top view. Its intention is to
 * ensure that the correct `effectivePipeline` is passed to mongot upon creation of the search
 * index.
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
coll.drop();

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

createSearchIndex(maxNestedView, {
    name: "maxNestedViewIndex",
    definition: {
        "mappings": {"dynamic": true},
        "fields": {"nested_tags": {"type": "string"}, "nested_category": {"type": "string"}}
    }
});

createSearchIndex(officePotentialView, {
    name: "officePotentialViewIndex",
    definition: {"mappings": {"dynamic": true}, "fields": {"office_potential": {"type": "string"}}}
});

// Queries to be ran on the maxNestedViewIndex.
let maxNestedViewTestQueries = [
    // Basic existence query on tags.
    {
        searchQuery: {$search: {index: "maxNestedViewIndex", exists: {path: "nested_tags"}}},
        validateFn: (results) => {
            assert(results.length == 5, "Existence query should return 5 results");

            for (let i = 0; i < results.length; ++i) {
                assert(results[i].nested_tags, "Results should have nested_tags");
                assert(results[i].nested_tags.length > 1, "Tags should be nested");
                assert(results[i].dependent_transformation == 35,
                       "Dependent transformation should have a value of 35.");
            }
        }
    },
    // Search for specific nested tag that should exist on all documents.
    {
        searchQuery: {
            $search: {index: "maxNestedViewIndex", text: {query: "level_18", path: "nested_tags"}}
        },
        validateFn: (results) => {
            assert(results.length == 5, "Deepest level tag search should return 5 results");

            for (let i = 0; i < results.length; ++i) {
                assert(results[i].nested_tags.includes("level_18"),
                       "Deepest level tag should be present");
                assert(results[i].dependent_transformation == 35,
                       "Dependent transformation should have a value of 35.");
            }
        }
    },
    // Search for specific nested category that should exist on some documents.
    {
        searchQuery: {
            $search: {
                index: "maxNestedViewIndex",
                text: {query: "large_level_18", path: "nested_category"}
            }
        },
        validateFn: (results) => {
            assert(results.length == 2, "Search for large_level_18 should return 2 documents");

            for (let i = 0; i < results.length; ++i) {
                assert(results[i].nested_category == "large_level_18",
                       "Nested category should be large_level_18");
                assert(results[i].dependent_transformation == 35,
                       "Dependent transformation should have a value of 35.");
            }
        }
    },
    // Search for specific category that should exist on some documents.
    {
        searchQuery:
            {$search: {index: "maxNestedViewIndex", text: {query: "medium", path: "category"}}},
        validateFn: (results) => {
            assert(results.length == 2, "Category medium should be present in 2 results");

            // Ensure that the deepest field still exists on these results.
            for (let i = 0; i < results.length; ++i) {
                assert(results[i].nested_tags.includes("level_18"),
                       "Deepest level tag should be present");
                assert(results[i].dependent_transformation == 35,
                       "Dependent transformation should have a value of 35.");
            }
        }
    },
];

// Queries to be ran on the officePotentialViewIndex.
let officePotentialViewTestQueries = [
    {
        searchQuery: {
            $search:
                {index: "officePotentialViewIndex", text: {query: "high", path: "office_potential"}}
        },
        validateFn: (results) => {
            assert(results.length == 1, "High office potential should only be present in 1 result");

            // Check all results with high office potential (just one at the moment).
            for (let i = 0; i < results.length; ++i) {
                assert(results[i].office_potential == "high", "Office potential should be high");
                assert(results[i].nested_tags.includes("tech"), "Nested tags should include tech");
                assert(results[i].nested_category == "large_level_17",
                       "Nested category should be large_level_17");
            }
        }
    },
    {
        searchQuery: {
            $search: {
                index: "officePotentialViewIndex",
                text: {query: "medium", path: "office_potential"}
            }
        },
        validateFn: (results) => {
            assert(results.length == 1,
                   "Medium office potential should only be present in 1 result");

            // Check all results with medium office potential (just one at the moment).
            for (let i = 0; i < results.length; ++i) {
                assert(results[i].office_potential == "medium",
                       "Office potential should be medium");
                assert(results[i].nested_tags.includes("tech"), "Nested tags should include tech");
                assert(results[i].nested_category == "medium_level_17",
                       "Nested category should be medium_level_17");
            }
        }
    },
    {
        searchQuery: {
            $search: {
                index: "officePotentialViewIndex",
                text: {query: "emerging", path: "office_potential"}
            }
        },
        validateFn: (results) => {
            assert(results.length == 1,
                   "Emerging office potential should only be present in 1 result");

            // Check all results with emerging office potential (just one at the moment).
            for (let i = 0; i < results.length; ++i) {
                assert(results[i].office_potential == "emerging",
                       "Office potential should be emerging");
                assert(results[i].nested_category == "large_level_17",
                       "Nested category should be large_level_17");
            }
        }
    },
    {
        searchQuery: {
            $search:
                {index: "officePotentialViewIndex", text: {query: "low", path: "office_potential"}}
        },
        validateFn: (results) => {
            assert(results.length == 2, "Low office potential should only be present in 2 results");

            // Check all results with low office potentials.
            for (let i = 0; i < results.length; ++i) {
                assert(results[i].office_potential == "low", "Office potential should be low");
            }
        }
    },
];

// Run and validate each test query for both views.
maxNestedViewTestQueries.forEach(({searchQuery, validateFn}) => {
    let results = maxNestedView.aggregate([searchQuery]).toArray();
    validateFn(results);
});

officePotentialViewTestQueries.forEach(({searchQuery, validateFn}) => {
    let results = officePotentialView.aggregate([searchQuery]).toArray();
    validateFn(results);
});

// Clean up search indexes.
dropSearchIndex(maxNestedView, {name: "maxNestedViewIndex"});
dropSearchIndex(officePotentialView, {name: "officePotentialViewIndex"});
