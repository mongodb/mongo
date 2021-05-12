/**
 * Test that DocumentSourceSetWindowFields errors when using more than the perscribed amount of
 * data. Memory checks are per node, so only test when the data is all in one place.
 */

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");                         // For FixtureHelpers.isMongos.
load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/discover_topology.js");                       // For findNonConfigNodes.

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1}))
        .featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db[jsTestName()];
coll.drop();

// Test that we can set the memory limit.
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       1200);

// Create a collection with enough documents in a single partition to go over the memory limit.
for (let i = 0; i < 10; i++) {
    coll.insert({_id: i, partitionKey: 1, str: "str"});
}

assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$setWindowFields: {sortBy: {partitionKey: 1}, output: {val: {$sum: "$_id"}}}}],
    cursor: {}
}),
                             5643011);

// The same query passes with a higher memory limit.
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       3150);
assert.commandWorked(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$setWindowFields: {sortBy: {partitionKey: 1}, output: {val: {$sum: "$_id"}}}}],
    cursor: {}
}));
// The query passes with multiple partitions of the same size.
for (let i = 0; i < 10; i++) {
    coll.insert({_id: i, partitionKey: 2, str: "str"});
}
assert.commandWorked(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields:
            {sortBy: {partitionKey: 1}, partitionBy: "$partitionKey", output: {val: {$sum: "$_id"}}}
    }],
    cursor: {}
}));

// Test that the query fails with a window function that stores documents.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            sortBy: {partitionKey: 1},
            partitionBy: "$partitionKey",
            output: {val: {$push: "$_id", window: {documents: [-9, 9]}}}
        }
    }],
    cursor: {}
}),
                             5414201);
// Reset limit for other tests.
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       100 * 1024 * 1024);
})();
