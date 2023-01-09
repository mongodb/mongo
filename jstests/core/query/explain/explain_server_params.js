// Tests server parameter information shown by explain.
// @tags: [
//   does_not_support_stepdowns,
//   # TODO SERVER-30466
//   does_not_support_causal_consistency,
//   requires_fcv_62,
// ]

(function() {
"use strict";

const coll = db.explain_server_params;
coll.drop();

assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({y: 1}));

let result = coll.explain().aggregate([{$match: {x: 1, y: 1}}]);

const expectedParamList = [
    'internalQueryFacetBufferSizeBytes',
    'internalLookupStageIntermediateDocumentMaxSizeBytes',
    'internalDocumentSourceGroupMaxMemoryBytes',
    'internalQueryMaxBlockingSortMemoryUsageBytes',
    'internalQueryProhibitBlockingMergeOnMongoS',
    'internalQueryFacetMaxOutputDocSizeBytes',
    'internalQueryMaxAddToSetBytes',
    'internalQueryFrameworkControl',
];

assert(result.hasOwnProperty('serverParameters'), result);
assert.hasFields(result.serverParameters, expectedParamList);

result = coll.find({x: 1, y: 1}).explain('executionStats');
assert(result.hasOwnProperty('serverParameters'), result);
assert.hasFields(result.serverParameters, expectedParamList);

result = coll.find({x: 1, y: 1}).explain('queryPlanner');
assert(result.hasOwnProperty('serverParameters'), result);
assert.hasFields(result.serverParameters, expectedParamList);
})();
