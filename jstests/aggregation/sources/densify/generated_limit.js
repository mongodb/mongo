/**
 * Test that densify correctly limits the number of generated documents.
 * @tags: [
 *   # Needed as $densify is a 51 feature.
 *   requires_fcv_51,
 * ]
 */

(function() {
"use strict";

load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameterOnAllHosts.
load("jstests/libs/discover_topology.js");                       // For findNonConfigNodes.

const paramName = "internalQueryMaxAllowedDensifyDocs";
const origParamValue = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalQueryMaxAllowedDensifyDocs: 1}))[paramName];
function setMaxDocs(max) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), paramName, max);
}
const coll = db[jsTestName()];
coll.drop();

function runAggregate(densifyStage, failCode = null) {
    if (failCode === null) {
        return db.runCommand({aggregate: coll.getName(), pipeline: [densifyStage], cursor: {}});
    } else {
        assert.commandFailedWithCode(
            db.runCommand({aggregate: coll.getName(), pipeline: [densifyStage], cursor: {}}),
            failCode);
    }
}

// Test that with explicit range and no documents we can't generate more than the limit.
setMaxDocs(10);
runAggregate({$densify: {field: "val", range: {step: 1, bounds: [0, 11]}}}, 5897900);
runAggregate({$densify: {field: "val", range: {step: 4, bounds: [0, 45]}}}, 5897900);
// Exactly ten documents should pass.
runAggregate({$densify: {field: "val", range: {step: 1, bounds: [0, 10]}}});

// Full fails if there are enough points between min and max to pass limit
assert.commandWorked(coll.insert({val: 0, part: 1}));
assert.commandWorked(coll.insert({val: 12, part: 1}));
runAggregate({$densify: {field: "val", range: {step: 1, bounds: "full"}}}, 5897900);

// Test that count is shared across partitions.
setMaxDocs(20);
assert.commandWorked(coll.insert({val: 0, part: 2}));
assert.commandWorked(coll.insert({val: 12, part: 2}));
runAggregate(
    {$densify: {field: "val", partitionByFields: ["part"], range: {step: 1, bounds: "partition"}}},
    5897900);

// Test that already existing documents don't count towards the limit.
coll.drop();
setMaxDocs(10);
assert.commandWorked(coll.insert({val: 0, part: 1}));
assert.commandWorked(coll.insert({val: 12, part: 1}));
runAggregate({$densify: {field: "val", range: {step: 1, bounds: "full"}}}, 5897900);
assert.commandWorked(coll.insert({val: 5, part: 1}));
runAggregate({$densify: {field: "val", range: {step: 1, bounds: "full"}}});

// Reset parameter.
setMaxDocs(origParamValue);
})();
