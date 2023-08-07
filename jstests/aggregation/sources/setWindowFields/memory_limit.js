/**
 * Test that DocumentSourceSetWindowFields errors when using more than the perscribed amount of
 * data. Memory checks are per node, so only test when the data is all in one place.
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {checkSBEEnabled} from "jstests/libs/sbe_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

// TODO SERVER-78709: Implement spilling
if (checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    jsTestLog("Skipping the test since spilling is not implemented in SBE yet");
    quit();
}

const coll = db[jsTestName()];
coll.drop();

// Test that we can set the memory limit.
const nonConfigNodes = DiscoverTopology.findNonConfigNodes(db.getMongo());
setParameterOnAllHosts(nonConfigNodes, "internalDocumentSourceSetWindowFieldsMaxMemoryBytes", 1200);

// Create a collection with enough documents in a single partition to go over the memory limit.
const docsPerPartition = 10;
for (let i = 0; i < docsPerPartition; i++) {
    assert.commandWorked(coll.insert({_id: i, partitionKey: 1, largeStr: Array(1025).toString()}));
}

assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$setWindowFields: {sortBy: {partitionKey: 1}, output: {val: {$sum: "$_id"}}}}],
    cursor: {},
    allowDiskUse: false
}),
                             5643011);

// The same query passes with a higher memory limit. Note that the amount of memory consumed by the
// stage is roughly double the size of the documents since each document has an internal cache.
const perDocSize = 1200;
setParameterOnAllHosts(nonConfigNodes,
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       (perDocSize * docsPerPartition * 3) + 1024);
assert.commandWorked(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$setWindowFields: {sortBy: {partitionKey: 1}, output: {val: {$sum: "$largeStr"}}}}],
    cursor: {},
    allowDiskUse: false
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
    cursor: {},
    allowDiskUse: false
}));

setParameterOnAllHosts(nonConfigNodes,
                       "internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                       (perDocSize * docsPerPartition) + 1024);

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
    cursor: {},
    allowDiskUse: false
}),
                             [5643011, 5414201]);
// Reset limit for other tests.
setParameterOnAllHosts(
    nonConfigNodes, "internalDocumentSourceSetWindowFieldsMaxMemoryBytes", 100 * 1024 * 1024);