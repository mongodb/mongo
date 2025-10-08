/**
 * Tests a few aggregation stages on views created from viewless timeseries collections
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

const timeFieldName = "time";
const metaFieldName = "status";
const bucketMaxSpanSeconds = 3600;
const viewName = jsTestName() + "_view";

/*
 * Creates a timeseries collection, populates it with `docs`, creates a view on the collection,
 * runs the aggregation `pipeline` on that view, and ensures that the result set is equal to the
 * result set created by running the same `pipeline` directly on the collection.
 */
function runTest(docs, queryPipeline, expectedResults, viewPipeline) {
    // Setup our DB & our collections.
    const tsColl = db.getCollection(jsTestName());
    tsColl.drop();
    db[viewName].drop();

    assert.commandWorked(
        db.createCollection(tsColl.getName(), {
            timeseries: {
                timeField: timeFieldName,
                metaField: metaFieldName,
                bucketMaxSpanSeconds: bucketMaxSpanSeconds,
                bucketRoundingSeconds: bucketMaxSpanSeconds,
            },
        }),
    );

    // Populate the collection with documents.
    assert.commandWorked(tsColl.insertMany(docs));

    // Create a view on the collection.
    assert.commandWorked(db.createView(viewName, jsTestName(), viewPipeline));

    const results = db[viewName].aggregate(queryPipeline).toArray();

    assert.sameMembers(expectedResults, results);
}

const dataset = [
    {
        _id: 1,
        name: "Alice",
        age: 25,
        status: "active",
        [timeFieldName]: new Date("2025-09-01T13:00:00"),
    },
    {
        _id: 2,
        name: "Bob",
        age: 32,
        status: "active",
        [timeFieldName]: new Date("2025-09-01T13:30:00"),
    },
    {
        _id: 3,
        name: "Carol",
        age: 29,
        status: "active",
        [timeFieldName]: new Date("2025-09-01T14:00:00"),
    },
    {
        _id: 4,
        name: "David",
        age: 34,
        status: "inactive",
        [timeFieldName]: new Date("2025-09-01T14:30:00"),
    },
    {
        _id: 5,
        name: "Eve",
        age: 28,
        status: "inactive",
        [timeFieldName]: new Date("2025-09-01T15:00:00"),
    },
    {
        _id: 6,
        name: "Frank",
        age: 41,
        status: "inactive",
        [timeFieldName]: new Date("2025-09-01T15:30:00"),
    },
];

// Test $match on a small dataset with an empty view pipeline.
let pipeline = [{$match: {status: "active"}}];
let expectedResults = [dataset[0], dataset[1], dataset[2]];
runTest(dataset, pipeline, expectedResults);

// Test $match on a small dataset with a view pipeline.
// This tests that the view is resolved before the $_internalUnpackBucket stage is appended.
// If instead the $_internalUnpackBucket stage were appended before resolving the view,
// the query would return no results due to the fact that the field "age" doesn't exist
// in the bucket documents and thus no matches would be found.
let viewPipeline = [{$match: {age: {$gt: 27}}}];
expectedResults = [dataset[2], dataset[1]];
runTest(dataset, pipeline, expectedResults, viewPipeline);

// Test $group and $average on a small dataset with an empty view pipeline.
pipeline = [{$group: {_id: "$status", averageAge: {$avg: "$age"}}}];
expectedResults = [
    {_id: "active", averageAge: 28.666666666666668},
    {_id: "inactive", averageAge: 34.333333333333336},
];
runTest(dataset, pipeline, expectedResults);

// Test $group and $average on a small dataset with a view pipeline.
viewPipeline = [{$match: {status: "inactive"}}];
expectedResults = [{_id: "inactive", averageAge: 34.333333333333336}];
runTest(dataset, pipeline, expectedResults, viewPipeline);

// Test $sort on a small dataset with an empty view pipeline.
pipeline = [{$sort: {age: -1}}];
expectedResults = [dataset[5], dataset[3], dataset[1], dataset[2], dataset[4], dataset[0]];
runTest(dataset, pipeline, expectedResults);

// Test $sort on a small dataset with a view pipeline.
viewPipeline = [{$project: {age: 1}}];
expectedResults = [];
dataset.forEach((doc) => expectedResults.push({_id: doc["_id"], age: doc["age"]}));
runTest(dataset, pipeline, expectedResults, viewPipeline);

// Test $project on a small dataset with an empty view pipeline.
pipeline = [{$project: {_id: 0, name: 1, age: 1}}];
expectedResults = [];
dataset.forEach((doc) => expectedResults.push({name: doc["name"], age: doc["age"]}));
runTest(dataset, pipeline, expectedResults);

// Test $project on a small dataset with a view pipeline.
viewPipeline = [{$match: {age: {$lt: 30}}}];
expectedResults = [];
dataset.forEach((doc) => {
    if (doc["age"] < 30) {
        expectedResults.push({name: doc["name"], age: doc["age"]});
    }
});
runTest(dataset, pipeline, expectedResults, viewPipeline);

// TODO SERVER-103133 Add tests for pipelines with sub-pipelines, including $unionWith.

// Test $lookup on a foreign collection that is a view on a timeseries collection.
{
    const normalCollName = jsTestName() + "_normal";
    const normalColl = db.getCollection(normalCollName);
    const tsColl = db.getCollection(jsTestName() + "_timeseries");
    tsColl.drop();
    normalColl.drop();
    db[viewName].drop();

    const normalDocs = [
        {name: "Alice", key: "active"},
        {name: "Bob", key: "inactive"},
        {name: "Carol", key: "active"},
    ];
    assert.commandWorked(normalColl.insertMany(normalDocs));

    // Create a timeseries collection and view.
    assert.commandWorked(
        db.createCollection(tsColl.getName(), {
            timeseries: {timeField: timeFieldName},
        }),
    );
    const tsDocs = [
        {_id: 1, status: "active", time: new Date("2025-09-01T13:00:00"), age: 25},
        {_id: 2, status: "inactive", time: new Date("2025-09-01T14:00:00"), age: 30},
        {_id: 3, status: "active", time: new Date("2025-09-01T15:00:00"), age: 35},
    ];
    assert.commandWorked(tsColl.insertMany(tsDocs));
    assert.commandWorked(db.createView(viewName, tsColl.getName(), [{$match: {age: {$lte: 30}}}]));

    // Validate $lookup.
    let pipeline = [
        {
            $lookup: {
                from: viewName,
                localField: "key",
                foreignField: "status",
                as: "matched",
            },
        },
        {$project: {_id: 0, key: 0, "matched.time": 0}},
    ];
    let expectedResults = [
        {name: "Alice", matched: [{_id: 1, status: "active", age: 25}]},
        {name: "Bob", matched: [{_id: 2, status: "inactive", age: 30}]},
        {name: "Carol", matched: [{_id: 1, status: "active", age: 25}]},
    ];
    let results = normalColl.aggregate(pipeline).toArray();
    assertArrayEq({
        actual: results,
        expected: expectedResults,
        extraErrorMsg: "Unexpected results with $lookup on a view on a timeseries collection",
    });

    // Validate $graphLookup.
    pipeline = [
        {
            $graphLookup: {
                from: viewName,
                startWith: "$key",
                connectFromField: "key",
                connectToField: "status",
                as: "matched",
                maxDepth: 0,
            },
        },
        {$project: {_id: 0, "matched.time": 0}},
    ];
    expectedResults = [
        {name: "Alice", key: "active", matched: [{_id: 1, status: "active", age: 25}]},
        {name: "Bob", key: "inactive", matched: [{_id: 2, status: "inactive", age: 30}]},
        {name: "Carol", key: "active", matched: [{_id: 1, status: "active", age: 25}]},
    ];
    results = normalColl.aggregate(pipeline).toArray();
    assertArrayEq({
        actual: results,
        expected: expectedResults,
        extraErrorMsg: "Unexpected results with $graphLookup on a view on a timeseries collection",
    });
}
