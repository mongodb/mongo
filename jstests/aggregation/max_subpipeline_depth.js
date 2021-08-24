// Tests that a pipeline isn't allowed to specify an arbitrary number of sub-pipelines within
// $lookups and other similar stages.
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isSharded.

const coll = db.max_subpipeline_depth;
coll.drop();

function makeLookupNDeep(n) {
    const lookupTemplate = {$lookup: {from: coll.getName(), as: "results", pipeline: []}};

    let lookupSpec = lookupTemplate;
    for (let i = 0; i < n; ++i) {
        lookupSpec = {$lookup: Object.merge(lookupTemplate.$lookup, {pipeline: [lookupSpec]})};
    }
    return lookupSpec;
}

function makeUnionNDeep(n) {
    const unionTemplate = {$unionWith: {coll: coll.getName(), pipeline: []}};

    let unionSpec = unionTemplate;
    for (let i = 0; i < n; ++i) {
        unionSpec = {$unionWith: Object.merge(unionTemplate.$unionWith, {pipeline: [unionSpec]})};
    }
    return unionSpec;
}

const maxDepth = 20;

assert.commandWorked(db.runCommand(
    {aggregate: coll.getName(), pipeline: [makeUnionNDeep(maxDepth - 1)], cursor: {}}));
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: [makeUnionNDeep(maxDepth)], cursor: {}}),
    ErrorCodes.MaxSubPipelineDepthExceeded);

// Do not run the rest of the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(coll) && !isShardedLookupEnabled) {
    return;
}

assert.commandWorked(db.runCommand(
    {aggregate: coll.getName(), pipeline: [makeLookupNDeep(maxDepth - 1)], cursor: {}}));
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: [makeLookupNDeep(maxDepth)], cursor: {}}),
    ErrorCodes.MaxSubPipelineDepthExceeded);
}());
