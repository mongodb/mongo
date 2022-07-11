// Tests that a pipeline isn't allowed to specify an arbitrary number of sub-pipelines within
// $lookups and other similar stages.
(function() {
"use strict";

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

assert.commandWorked(db.runCommand(
    {aggregate: coll.getName(), pipeline: [makeLookupNDeep(maxDepth - 1)], cursor: {}}));
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: [makeLookupNDeep(maxDepth)], cursor: {}}),
    ErrorCodes.MaxSubPipelineDepthExceeded);
}());
