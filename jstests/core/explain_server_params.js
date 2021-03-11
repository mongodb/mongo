// Tests server parameter information shown by explain.
// @tags: [
//   requires_fcv_49,
//   does_not_support_stepdowns
// ]

(function() {
"use strict";

const coll = db.explain_server_params;
coll.drop();

assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({y: 1}));

let result = coll.explain().aggregate([{$match: {x: 1, y: 1}}]);

assert(result.hasOwnProperty('serverParameters'), result);
assert.hasFields(result.serverParameters, [
    'internalQueryFacetBufferSizeBytes',
    'internalLookupStageIntermediateDocumentMaxSizeBytes',
    'internalDocumentSourceGroupMaxMemoryBytes',
    'internalQueryMaxBlockingSortMemoryUsageBytes',
    'internalQueryProhibitBlockingMergeOnMongoS',
    'internalQueryFacetMaxOutputDocSizeBytes',
    'internalQueryMaxAddToSetBytes'
]);

result = coll.find({x: 1, y: 1}).explain('executionStats');
assert(result.hasOwnProperty('serverParameters'), result);
assert.hasFields(result.serverParameters, [
    'internalQueryFacetBufferSizeBytes',
    'internalLookupStageIntermediateDocumentMaxSizeBytes',
    'internalDocumentSourceGroupMaxMemoryBytes',
    'internalQueryMaxBlockingSortMemoryUsageBytes',
    'internalQueryProhibitBlockingMergeOnMongoS',
    'internalQueryFacetMaxOutputDocSizeBytes',
    'internalQueryMaxAddToSetBytes'
]);

result = coll.find({x: 1, y: 1}).explain('queryPlanner');
assert(result.hasOwnProperty('serverParameters'), result);
assert.hasFields(result.serverParameters, [
    'internalQueryFacetBufferSizeBytes',
    'internalLookupStageIntermediateDocumentMaxSizeBytes',
    'internalDocumentSourceGroupMaxMemoryBytes',
    'internalQueryMaxBlockingSortMemoryUsageBytes',
    'internalQueryProhibitBlockingMergeOnMongoS',
    'internalQueryFacetMaxOutputDocSizeBytes',
    'internalQueryMaxAddToSetBytes'
]);
})();
