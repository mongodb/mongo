/**
 * Test that DocumentSourceSetWindowFields errors when using more than the perscribed amount of
 * data. Memory checks are per node, so only test when the data is all in one place.
 */

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");                         // For FixtureHelpers.isMongos.
load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/discover_topology.js");                       // For findNonConfigNodes.

const coll = db[jsTestName()];
coll.drop();

// Test that we can set the memory limit.
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       1200);

// Create a collection with enough documents in a single partition to go over the memory limit.
const docsPerPartition = 10;
for (let i = 0; i < docsPerPartition; i++) {
    assert.commandWorked(coll.insert({_id: i, partitionKey: 1, largeStr: Array(1025).toString()}));
}

assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$setWindowFields: {sortBy: {partitionKey: 1}, output: {val: {$sum: "$_id"}}}}],
    cursor: {}
}),
                             5643011);

// The same query passes with a higher memory limit. Note that the amount of memory consumed by the
// stage is roughly double the size of the documents since each document has an internal cache.
const perDocSize = 1200;
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       (perDocSize * docsPerPartition * 3) + 1024);
assert.commandWorked(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$setWindowFields: {sortBy: {partitionKey: 1}, output: {val: {$sum: "$largeStr"}}}}],
    cursor: {}
}));
// The query passes with multiple partitions of the same size.
for (let i = docsPerPartition; i < docsPerPartition * 2; i++) {
    assert.commandWorked(coll.insert({_id: i, partitionKey: 2, largeStr: Array(1025).toString()}));
}
assert.commandWorked(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            sortBy: {partitionKey: 1},
            partitionBy: "$partitionKey",
            output: {val: {$sum: "$largeStr"}}
        }
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
            output: {val: {$max: "$largeStr", window: {documents: [-9, 9]}}}
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
