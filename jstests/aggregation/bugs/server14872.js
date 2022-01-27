// SERVER-14872: Aggregation expression to concatenate multiple arrays into one

(function() {
'use strict';

load('jstests/aggregation/extras/utils.js');        // For assertErrorCode.
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

var coll = db.agg_concat_arrays_expr;
coll.drop();

assert.commandWorked(coll.insert({a: [1, 2], b: ['three'], c: [], d: [[3], 4], e: null, str: 'x'}));

// Basic concatenation.
var pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a', '$b', '$c']}}}];
assert.eq(coll.aggregate(pipeline).toArray(), [{all: [1, 2, 'three']}]);

// Concatenation with nested arrays.
pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a', '$d']}}}];
assert.eq(coll.aggregate(pipeline).toArray(), [{all: [1, 2, [3], 4]}]);

// Concatenation with 1 argument.
pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a']}}}];
assert.eq(coll.aggregate(pipeline).toArray(), [{all: [1, 2]}]);

// Any nullish inputs will result in null.
pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a', '$e']}}}];
assert.eq(coll.aggregate(pipeline).toArray(), [{all: null}]);
pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a', '$f']}}}];
assert.eq(coll.aggregate(pipeline).toArray(), [{all: null}]);

// Error on any non-array, non-null inputs.
pipeline = [{$project: {_id: 0, all: {$concatArrays: ['$a', '$str']}}}];
assertErrorCode(coll, pipeline, 28664);
}());
