// Created as part of SERVER-83765
// ref: https://www.mongodb.com/docs/manual/core/link-text-indexes/

const coll = db.match_on_meta_exclusion_projection;
coll.drop();

assert.commandWorked(coll.insertMany([
    {
        _id: 1,
        content: "This morning I had a cup of coffee.",
        about: "beverage",
        keywords: ["coffee"]
    },
    {
        _id: 2,
        content: "Who likes chocolate ice cream for dessert?",
        about: "food",
        keywords: ["poll"]
    },
    {
        _id: 3,
        content: "My favorite flavors are strawberry and coffee",
        about: "ice cream",
        keywords: ["food", "dessert"]
    }
]));

assert.commandWorked(coll.createIndex({"content": "text"}));

// Verifying that, for a pipeline containing:
//  - An EXCLUSION projection which contains $meta
//  - A subsequent dependent $match stage
// $match is not pushed ahead of $project.

// Note that $meta is the ONLY expression permitted in an exclude projection.
const projectMetaExclude = [
    {$match: {$text: {$search: "java coffee shop"}}},
    {$project: {"about": 0, "score": {"$meta": "textScore"}}},
    {$match: {"score": {"$gt": 0.65}}}
];

// The correct output of the projectMetaExclude pipeline.
// If $match WAS incorrectly pushed in front of $project, the output would be 0 documents.
const excludeExpected = [{
    "_id": 1,
    "content": "This morning I had a cup of coffee.",
    "keywords": ["coffee"],
    "score": 0.6666666666666666
}];

const excludeActual = coll.aggregate(projectMetaExclude).toArray();
assert.eq(excludeActual, excludeExpected);

// For the purpose of comparison, verifying that, for a pipeline containing:
//  - An INCLUSION projection which contains $meta (`"about": 0` has been removed)
//  - A subsequent dependent $match stage
// $match is not pushed ahead of $project.
const projectMetaInclude = [
    {$match: {$text: {$search: "java coffee shop"}}},
    {$project: {"score": {"$meta": "textScore"}}},
    {$match: {"score": {"$gt": 0.65}}}
];

// The correct output of the projectMetaInclude pipeline. If $match WAS pushed ahead of $project,
// the output would be 0 documents.
const includeExpected = [{"_id": 1, "score": 0.6666666666666666}];

const includeActual = coll.aggregate(projectMetaInclude).toArray();
assert.eq(includeActual, includeExpected);
